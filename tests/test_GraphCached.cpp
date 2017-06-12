#include <sys/mman.h>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

#include "GraphCached.h"

using namespace graphcached;
const int IOSIZE = 24*1024*1024;
class Block : public DiskComponent{
public:
	Block(): DiskComponent() {
	}
};
class TestGraph : public GraphCached<int, Block> {
public:
	TestGraph(std::string filename): GraphCached(filename) {}
	DiskSegmentInfo plocate(int key) {
		int fd = open(baseFilename.c_str(), O_RDONLY);
		DiskSegmentInfo dsi(fd, key*IOSIZE, IOSIZE);
		//off_t offset = key * IOSIZE;
		//ssize_t bytes = 0;
		//while( bytes < IOSIZE) {
		//	ssize_t cur = pread(fd, value.getBuffer(), 1024, bytes + offset);
		//	bytes += cur;
		//	if (cur == 0) break;
		//}
		//close(fd);
		return dsi;
	}
};

int main(int argc, char* argv[]) {
        GraphCached_init(argc, argv);
	std::cout<< "graphcached initialized" << std::endl;
	std::string filename = "../data/soc-LiveJournal.txt"; 
	TestGraph graph(filename);
	Block* firstBlock = graph.read(0);
	
	std::cout<<firstBlock->data()<<std::endl;
	std::cout<<"first line: ";
	char* data = reinterpret_cast<char*>(firstBlock->data());
	int i = 0; 
	while ('\n' != data[i])
	    std::cout<<data[i++];
	std::cout<<std::endl;
	
	return 0;
}
