#include <string>
// GraphCached user code
// class Graph : public GraphCached<int, Partition> {
//
// };

// GraphCached code

namespace graphcached {

template <class KeyTy, class ValueTy>
class GraphCached {
private:
	std::string baseFilename;
public:
	GraphCached(std::string filename): baseFilename(filename){
	}
	~GraphCached(){}
	ValueTy& read(KeyTy& key, int cacheFlag = 1);
	int write(KeyTy& key, ValueTy& value, int wbFlag = 0);
	virtual int readFromFile(KeyTy& key, ValueTy& value) = 0;
	virtual int writeToFile(KeyTy& key, ValueTy& value) = 0;
};

} // namespace graphcached
