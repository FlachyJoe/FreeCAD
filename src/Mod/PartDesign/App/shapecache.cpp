#include <iostream>

#include "shapecache.h"

namespace ShapeCache {

_ShapeCache::_ShapeCache()
{
    sMap = TopTools_IndexedMapOfShape(MAX_SIZE);
    hMap  = boost::bimap<std::string, int>();
}

// Get a shape by its unique id
TopoDS_Shape _ShapeCache::loadShape(int id)
{
    TopoDS_Shape theShape = sMap(id);
    return theShape;
}

// create an Uid for a shape
int _ShapeCache::getUid(TopoDS_Shape shape){
    if (sMap.Contains(shape)){
        return sMap.FindIndex(shape);
    }
    return sMap.Add(shape);
}

int _ShapeCache::getUid(std::string params){

    int id;
    try{
        id = hMap.right.find(params);
    }
    catch(Standard_Failure){
        id = hMap.size() + 1;
        hMap.left.insert(HashMap::value_type(id, params));
    }

}

}
