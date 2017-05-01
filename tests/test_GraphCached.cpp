#include <sys/mman.h>
#include <fcntl.h>
#include <iostream>

#include "GraphCached.h"

using namespace GraphCached;
const int IOSIZE = 24*1024*1024;
class Block {
	void* buffer;
};
class TestGraph : public GraphCached<int, Block> {
public:
	TestGraph(string filename): GraphCached(filename) {}
	int readFromFile(int& key, Block& value) {
		int fd = open(baseFilename, O_RDONLY);
		ssize_t offset = key * IOSIZE;
		ssize_t bytes = 0;
		while( bytes < IOSIZE) {
			ssize_t cur = pread(fd, value.buffer, 1024, bytes + offset);
			bytes += cur;
			if (cur == 0) break;
		}
		close(fd);
	}
	int writeToFile(int& key, Block& value) {
		
	}
}

int main(int argc, char* argv[]) {
	string filename = "../data/soc-LiveJournal1.txt"; 
	TestGraph graph(filename);
	Block& firstBlock = graph.read(0);
	std::cout<< (char*)(firstBlock.buffer)[0]<< std::endl;
	return 0;
}
