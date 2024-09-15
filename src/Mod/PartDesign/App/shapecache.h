#ifndef SHAPECACHE_H
#define SHAPECACHE_H

#include <boost/bimap.hpp>

#include <TopoDS_Shape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

namespace ShapeCache{

const int MAX_SIZE = 4096;
typedef boost::bimap<int, std::string> HashMap;

class _ShapeCache
{
public:
    _ShapeCache();
    bool isCached(std::string uid);
    TopoDS_Shape loadShape(int uid);
    void storeShape(int uid, TopoDS_Shape shape);
    int getUid(TopoDS_Shape shape);
    int getUid(std::string params);

private:
    TopTools_IndexedMapOfShape sMap;
    HashMap hMap;
};

_ShapeCache ShapeCache();

} // namespace
#endif  // SHAPECACHE_H
