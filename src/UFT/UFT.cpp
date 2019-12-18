#include "UFT.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <thread>
#include <random>
#include <time.h>
#include <queue>
#include <unordered_map>
#include <vector>

/*
The size of the UDP datagrams. Set to 508 to minimize fragmentation but avoid overhead. This value
must be the same on the client and server.
*/
#define DATAGRAM_SIZE 1016
#define BLOCK_SIZE (DATAGRAM_SIZE - sizeof(struct uft_header))
#define ACK_SIZE (DATAGRAM_SIZE - sizeof(struct uft_header)) / sizeof(uint32_t) 

#define ACK_FREQUENCY 10

#define DGTYPE_DATA 1
#define DGTYPE_ACK 2

#define FLAG_COMPLETE 1 << 0
#define FLAG_ABORT 1 << 1

/*
Header struct. This struct must have the same size and padding on the client and server. Most
architectures and compilers will arrange and pad this struct identically (and the order of the
elements was carefully chosen to minimize padding), but it is possible that a compiler may do
this differently. Ideally this should be replaced with a serialization and deserialization
method anyway.
*/
struct uft_header {
	/*
	Generated at the beginning of the transfer and never changed. Potentially used for
	multiplexing in the future.
	*/
	uint32_t transfer_id;
	/*
	Either DGTYPE_DATA or DGTYPE_ACK
	*/
	uint8_t datagram_type;
	/*
	Binary OR of various flags
	*/
	uint8_t flags;
	/*
	Used to store the number of bytes in the final block being transferred. This allows files
	with sizes that are not a multiple of the block size to be transferred.
	*/
	uint16_t data_only;
	/*
	If the type is DGTYPE_DAT then the top 32 bits of this is the total number of blocks to be
	transferred and the bottom 32 bits is the block number of the current block.

	If the type is DGTYPE_ACK then this is the number of missing blocks.
	*/
	uint64_t type_specific;
};

uint32_t randint(){
	std::random_device rnd;
	std::mt19937_64 mt(rnd());
	std::uniform_int_distribution<uint32_t> dis;

	return dis(mt);
}

void slp(long long l){
	struct timespec s;
	s.tv_sec = l / 1000;
	s.tv_nsec = 1000000 * (l % 1000);
	nanosleep(&s, NULL);
}

void send_packet(int fd, struct uft_header &header, char *data, size_t data_size, bool progress,
	struct sockaddr *srv, socklen_t addr_len){
	char buf[DATAGRAM_SIZE];

	// Fill the buffer with the data
	memcpy(buf, &header, sizeof(struct uft_header));
	if(data != NULL){
		memcpy(buf + sizeof(struct uft_header), data, data_size);
	}
	
	int written = 0;
	while(written != sizeof(struct uft_header) + data_size){
		int ok = sendto(fd, buf, sizeof(struct uft_header) + data_size,
			progress ? MSG_CONFIRM : 0, srv, addr_len);
		if(ok < 0){
			if(errno == EPERM){
				/*
				Configuring iptables to drop outgoing packets on the server causes EPERM to
				be returned (and apparently this isn't being changed for legacy reasons).
				Adding this in will prevent this error from causing crashes, but on the other
				hand it will also prevent actual causes of EPERM to be detected. Comment this
				entire if statement out to change the behavior.
				*/
				return;
			}
			if(errno != EINTR){
				perror("Socket send error!\n");
				exit(1);
			}
		}
		else{
			written += ok;
		}
	}
	return;
}

void write_to_file(int fd, std::vector<char *> &blocks, uint32_t &removed_blocks, int expected, 
	int last_block){
	/*
	Repeatedly wait for a block to be available and write it to file
	*/
	for(int i = 0; i < blocks.size(); i++){
		if(blocks[i] == NULL){
			blocks.erase(blocks.begin(), blocks.begin() + i);
			removed_blocks += i;
			return;
		}

		char *to_write = blocks[i];

		/*
		Attempt to write to file
		*/
		int written = 0;
		int expected_bytes = ((removed_blocks + i) == expected - 1) ? last_block : 
			(DATAGRAM_SIZE - sizeof(struct uft_header));

		while(written != expected_bytes){
			int out = write(fd, to_write + sizeof(struct uft_header) + written,
				expected_bytes - written);
			if(out == -1){
				perror("Write failed\n");
				exit(1);
			}
			else{
				written += out;
			}
		}

		free(to_write);
	}
	removed_blocks += blocks.size();
	blocks.clear();
}

long long time_milli(){
	struct timespec s;
	clock_gettime(CLOCK_REALTIME, &s);
	return 1000LL * s.tv_sec + (s.tv_nsec / 1000000);
}

uint16_t generate_ack(uint32_t last_full, uint32_t *data, 
	std::unordered_map<uint32_t, long long> &m, long long rtt){
	data[0] = last_full;
	uint16_t inserts = 0;
	long long tm = time_milli();
	for(auto e : m){
		if(tm - e.second >= rtt * 10){
			data[++inserts] = e.first;
			//printf("Reqd: %d\n", e.first);
		}
		if(inserts + 1 == ACK_SIZE){
			break;
		}
	}
	for(int i = 0; i < inserts; i++){
		m[data[i+1]] = tm;
	}
	return inserts;
}

void insert_into_shortened_vector(uint32_t start, uint32_t i, char *p, std::vector<char *> &v,
	std::unordered_map<uint32_t, long long> &last_reqs, uint32_t total_reqs){
	//printf("Got: %d\n", i);
	uint32_t in = i - start;
	if(in < v.size()){
		v[in] = p;
		last_reqs.erase(i);
		if(in == v.size() - 1 && i != total_reqs - 1){
			last_reqs[start + (uint32_t) v.size()] = time_milli(); 
			v.push_back(NULL);
		}
	}
	else{
		long long t = time_milli();
		for(uint32_t j = start + v.size(); j < i; j++){
			v.push_back(NULL);
			last_reqs[j] = t;
		}
		v.push_back(p);
		if(i != total_reqs - 1){
			last_reqs[start + (uint32_t) v.size()] = time_milli(); 
			v.push_back(NULL);
		}
	}
}

bool run_server(int fd, int port, uint32_t max_size, long long rtt){

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock < 0){
		perror("Could not create socket\n");
		exit(1);
	}
	struct sockaddr_in serv_addr;
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);
	int ok = bind(sock, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr_in));
	if(ok < 0){
		perror("Could not bind socket\n");
		exit(1);
	}

	uint64_t transfer_id = 0;
	uint32_t expected_blocks = 0;
	uint16_t last_block_size = 0;

	uint32_t written_blocks = 0;
	std::vector<char *> unwritten;

	int ack_max_fields = (DATAGRAM_SIZE - sizeof(uft_header)) / sizeof(uint32_t);

	long long last_receive = 0;
	long long last_ack = 0;
	long long packets_sent = 0;
	long long packets_received = 0;
	long long data_bytes = 0;

	std::unordered_map<uint32_t, long long> last_request;

	struct sockaddr src_addr;
	socklen_t src_addr_len = sizeof(struct sockaddr);

	char *pkt = (char *) malloc(DATAGRAM_SIZE);
	ok = recvfrom(sock, pkt, DATAGRAM_SIZE, 0, &src_addr, &src_addr_len);
	printf("Client connected!\n");
	struct uft_header *h = (struct uft_header *) pkt;

	transfer_id = h->transfer_id;
	expected_blocks = h->type_specific >> 32;
	last_block_size = h->data_only;
	last_receive = time_milli();
	packets_received++;

	insert_into_shortened_vector(written_blocks, h->type_specific & 0xFFFFFFFF, pkt,
		unwritten, last_request, expected_blocks);
	write_to_file(fd, unwritten, written_blocks, expected_blocks, last_block_size);

	uint32_t res[ack_max_fields];
	uint16_t itd = generate_ack(written_blocks, res, last_request, rtt);

	struct uft_header resp_h;
	resp_h.transfer_id = transfer_id;
	resp_h.datagram_type = DGTYPE_ACK;
	resp_h.flags = expected_blocks == written_blocks ? FLAG_COMPLETE : 0;
	resp_h.type_specific = itd;

	send_packet(sock, resp_h, (char *) res, sizeof(struct uft_header) + sizeof(uint32_t) * (itd + 1),
		true, &src_addr, src_addr_len);
	data_bytes += sizeof(uint32_t) * (itd + 1);
	last_ack = time_milli();

	while(written_blocks != expected_blocks){
		pkt = (char *) malloc(DATAGRAM_SIZE);
		bool should_wait = false;
		int ok = recvfrom(sock, pkt, DATAGRAM_SIZE, MSG_DONTWAIT, &src_addr, &src_addr_len);
		if(ok < 0){
			if(errno != EAGAIN && errno != EWOULDBLOCK){
				perror("Bad recv\n");
				exit(1);
			}
			free(pkt);
			should_wait = true;
		}
		else{
			packets_received++;
			h = (struct uft_header *) pkt;
			insert_into_shortened_vector(written_blocks, h->type_specific & 0xFFFFFFFF, pkt,
				unwritten, last_request, expected_blocks);
			write_to_file(fd, unwritten, written_blocks, expected_blocks, last_block_size);
			last_receive = time_milli();
		}

		if(written_blocks == expected_blocks || last_ack + rtt <= time_milli()){
			// Send ack
			itd = generate_ack(written_blocks, res, last_request, rtt);

			resp_h.flags = expected_blocks == written_blocks ? FLAG_COMPLETE : 0;
			resp_h.type_specific = itd;
			data_bytes += sizeof(uint32_t) * (1 + itd);

			send_packet(sock, resp_h, (char *) res, 
				sizeof(struct uft_header) + sizeof(uint32_t) * (itd + 1),
				(written_blocks == expected_blocks) || (!should_wait), 
				&src_addr, src_addr_len);
			packets_sent++;
			last_ack = time_milli();
		}

		
		if(last_receive + 300 * rtt < time_milli()){
			for(auto e : unwritten){
				if (e != NULL){
					free(e);
				}
			}
			printf("Timeout!\n");
			printf("Sent %lld packets with %lld header bytes and %lld data bytes!\n", 
				packets_sent, packets_sent * sizeof(struct uft_header), data_bytes);
			printf("Received %lld packets with %lld header bytes and %lld data bytes!\n", 
				packets_received, packets_received * sizeof(struct uft_header),
				packets_received * BLOCK_SIZE);
			return 1;
		}
		if(should_wait){
			slp(rtt);
		}
	}
	printf("%lld, %lld, %lld\n", 
		packets_sent, packets_sent * sizeof(struct uft_header), data_bytes);
	printf("%lld, %lld, %lld\n", 
		packets_received, packets_received * sizeof(struct uft_header),
		packets_received * BLOCK_SIZE);

	return 0;
}

char *read_from_file(int fd){
	char *p = (char *) malloc(BLOCK_SIZE);
	int ok = read(fd, p, BLOCK_SIZE);
	if(ok == -1){
		perror("Read fail\n");
		exit(1);
	}

	return p;
}

void process_ack(uint32_t *b, std::queue<uint32_t> &retr, std::vector<char *> &unacked,
	uint32_t &unacked_start, uint64_t ack_length){

	if(b[0] > unacked_start){
		for(uint32_t i = 0; i < b[0] - unacked_start; i++){
			free(unacked[i]);
		}
		unacked.erase(unacked.begin(), unacked.begin() + b[0] - unacked_start);
		unacked_start = b[0];
	}
	for(int i = 0; i < ack_length; i++){
		retr.push(b[i+1]);
	}
}

bool run_client(int fd, struct sockaddr *remote, socklen_t remote_size, long long rtt, 
	int wait){
	struct stat stats;
	fstat(fd, &stats);
	uint32_t file_size = stats.st_size;

	uint32_t block_count = (file_size + BLOCK_SIZE - 1) / (BLOCK_SIZE);
	uint16_t last_block = file_size % (BLOCK_SIZE);
	if(last_block == 0){
		last_block = BLOCK_SIZE;
	}

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock == -1){
		perror("Could not create socket.\n");
		exit(1);
	}

	bool next_progress = true;
	bool wait_ack = false;

	std::queue<uint32_t> retransmits;
	std::vector<char *> unacked;
	uint32_t unacked_start = 0;

	struct uft_header h;
	h.transfer_id = randint();
	h.datagram_type = DGTYPE_DATA;
	h.data_only = last_block;
	h.flags = 0;
	h.type_specific = (uint64_t) block_count << 32;

	long long last_received = time_milli();

	long long data_bytes = 0;

	long long packets_sent = 0;
	long long packets_received = 0;
	while (unacked_start != block_count){
		uint32_t pkt;
		char *data = NULL;
		if(((unacked_start + unacked.size() == block_count) || packets_sent % 2 == 0)
				&& !retransmits.empty()){
			do{
				pkt = retransmits.front();
				retransmits.pop();
			} while(unacked_start > pkt && !retransmits.empty());

			if(unacked_start <= pkt){
				data = unacked[pkt - unacked_start];
				//printf("Retransmitting %d\n", pkt);
			}
		}
		else if(unacked_start + unacked.size() < block_count){
			pkt = unacked_start + unacked.size();
			data = read_from_file(fd);
			unacked.push_back(data);
		}
		else{
			wait_ack = true;
		}
		if(data != NULL){
			h.type_specific = (h.type_specific & 0xFFFFFFFF00000000) + pkt;

			send_packet(sock, h, data, BLOCK_SIZE, next_progress, remote, remote_size);
			packets_sent++;
			next_progress = false;
		}
		if(wait > 0){
			slp(wait);
		}

		char resp[DATAGRAM_SIZE];
		int ok = recvfrom(sock, resp, DATAGRAM_SIZE, wait_ack ? 0 : MSG_DONTWAIT, 
			remote, &remote_size);
		if(ok == -1){
			if(errno != EAGAIN && errno != EWOULDBLOCK){
				perror("Bad recv\n");
				exit(1);
			}
			
			if(last_received + 100 * rtt < time_milli()){
				for(auto e : unacked){
					free(e);
				}
				printf("Timeout!\n");
				printf("%lld, %lld, %lld\n",
					packets_sent,
					sizeof(struct uft_header) * packets_sent, BLOCK_SIZE * packets_sent);
				printf("%lld, %lld, %lld\n", 
					packets_received, sizeof(struct uft_header) * packets_received,
					data_bytes);
				return 1;
			}
		}
		else{
			packets_received++;
			last_received = time_milli();
			next_progress = true;
			uint64_t data_len = ((struct uft_header *)resp)->type_specific;
			data_bytes += (data_len + 1) * sizeof(uint32_t);
			process_ack((uint32_t *)(resp + sizeof(struct uft_header)), retransmits, unacked,
				unacked_start, data_len);
			if(((struct uft_header *)resp)->flags & FLAG_COMPLETE != 0){
				break;
			}
		}
	}
	printf("%lld, %lld, %lld\n", packets_sent,
		sizeof(struct uft_header) * packets_sent, BLOCK_SIZE * packets_sent);
	printf("%lld, %lld, %lld\n",
		packets_received, sizeof(struct uft_header) * packets_received, data_bytes);
	return 0;
}
