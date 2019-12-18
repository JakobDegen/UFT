GCC= g++
CFLAGS= -std=c++11 -g
OUTPUT_FOLDER=../bin

all: server client

dirs:
	if [ ! -d "${OUTPUT_FOLDER}" ]; then mkdir "${OUTPUT_FOLDER}"; fi

server: dirs
	$(GCC) $(CFLAGS) -o "$(OUTPUT_FOLDER)/server" simple_server.cpp UFT/UFT.cpp

client: dirs
	$(GCC) $(CFLAGS) -Dcg=10 -o "$(OUTPUT_FOLDER)/cg_client" simple_client.cpp UFT/UFT.cpp
	$(GCC) $(CFLAGS) -Dcg=0 -o "$(OUTPUT_FOLDER)/ncg_client" simple_client.cpp UFT/UFT.cpp

clean:
	rm -f "$(OUTPUT_FOLDER)/server" "$(OUTPUT_FOLDER)/cg_client" "$(OUTPUT_FOLDER)/ncg_client"
