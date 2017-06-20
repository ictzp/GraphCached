#include <sys/mman.h>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

#include "GraphCached.h"

using namespace graphcached;
const int IOSIZE = 24*1024*1024;
class Block : public DiskComponent<int>{
public:
	Block(): DiskComponent() {
	}
};
class TestGraph : public GraphCached<int, Block> {
public:
	TestGraph(std::string filename): GraphCached(filename) {}
	TestGraph(std::string filename, uint32_t clsp, uint64_t cs, uint32_t mcs): GraphCached(filename, clsp, cs, mcs){}
	DiskSegmentInfo plocate(int key) {
		int fd = open(baseFilename.c_str(), O_RDONLY);
		DiskSegmentInfo dsi(fd, key*IOSIZE, IOSIZE);
		return dsi;
	}
};

int main(int argc, char* argv[]) {
	std::cout<< "graphcached initialized" << std::endl;
	std::string filename = "/home/zhaopeng/graph/data/soc-LiveJournal1.txt"; 
	TestGraph graph(filename, 12u, 4096*1024*1024ull, 24*1024*1024u);
	Block* firstBlock = graph.read(0);
	
	std::cout<<firstBlock->addr<<std::endl;
	std::cout<<"first line: ";
	char* data = reinterpret_cast<char*>(firstBlock->addr);
	int i = 0; 
	while ('\n' != data[i])
	    std::cout<<data[i++];
	std::cout<<std::endl;
	
	return 0;
}
