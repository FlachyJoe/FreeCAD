/***************************************************************************
 *   Copyright (c) 2009 Jürgen Riegel <juergen.riegel@web.de>              *
 *   Copyright (c) 2017 Qingfeng Xia  <qingfeng.xia at oxford uni>         *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#ifndef _PreComp_
#include <Python.h>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>

#include <SMESHDS_Mesh.hxx>
#include <SMESH_Mesh.hxx>

#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkDataSetReader.h>
#include <vtkDataSetWriter.h>
#include <vtkDoubleArray.h>
#include <vtkHexahedron.h>
#include <vtkIdList.h>
#include <vtkLine.h>
#include <vtkPointData.h>
#include <vtkPyramid.h>
#include <vtkQuad.h>
#include <vtkQuadraticEdge.h>
#include <vtkQuadraticHexahedron.h>
#include <vtkQuadraticPyramid.h>
#include <vtkQuadraticQuad.h>
#include <vtkQuadraticTetra.h>
#include <vtkQuadraticTriangle.h>
#include <vtkQuadraticWedge.h>
#include <vtkTetra.h>
#include <vtkTriangle.h>
#include <vtkUnsignedCharArray.h>
#include <vtkUnstructuredGrid.h>
#include <vtkWedge.h>
#include <vtkXMLPUnstructuredGridReader.h>
#include <vtkXMLUnstructuredGridReader.h>
#include <vtkXMLUnstructuredGridWriter.h>
#endif

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/TimeInfo.h>
#include <Base/Type.h>

#include "FemAnalysis.h"
#include "FemResultObject.h"
#include "FemVTKTools.h"


namespace Fem
{

template<class TReader>
vtkDataSet* readVTKFile(const char* fileName)
{
    vtkSmartPointer<TReader> reader = vtkSmartPointer<TReader>::New();
    reader->SetFileName(fileName);
    reader->Update();
    auto output = reader->GetOutput();
    if (output) {
        output->Register(reader);
    }
    return vtkDataSet::SafeDownCast(output);
}

template<class TWriter>
void writeVTKFile(const char* filename, vtkSmartPointer<vtkUnstructuredGrid> dataset)
{
    vtkSmartPointer<TWriter> writer = vtkSmartPointer<TWriter>::New();
    writer->SetFileName(filename);
    writer->SetInputData(dataset);
    writer->Write();
}

namespace
{

// Helper function to fill vtkCellArray from SMDS_Mesh using vtk cell order
template<typename T, typename E>
void fillVtkArray(vtkSmartPointer<vtkCellArray>& elemArray, std::vector<int>& types, const E* elem)
{
    vtkSmartPointer<T> cell = vtkSmartPointer<T>::New();
    const std::vector<int>& order = SMDS_MeshCell::toVtkOrder(elem->GetEntityType());
    if (!order.empty()) {
        for (int i = 0; i < elem->NbNodes(); ++i) {
            cell->GetPointIds()->SetId(i, elem->GetNode(order[i])->GetID() - 1);
        }
    }
    else {
        for (int i = 0; i < elem->NbNodes(); ++i) {
            cell->GetPointIds()->SetId(i, elem->GetNode(i)->GetID() - 1);
        }
    }
    elemArray->InsertNextCell(cell);
    types.push_back(SMDS_MeshCell::toVtkType(elem->GetEntityType()));
}

// Helper function to fill SMDS_Mesh elements ID from vtk cell
void fillMeshElementIds(vtkCell* cell, std::vector<int>& ids)
{
    VTKCellType cellType = static_cast<VTKCellType>(cell->GetCellType());
    const std::vector<int>& order = SMDS_MeshCell::fromVtkOrder(cellType);
    vtkIdType* vtkIds = cell->GetPointIds()->GetPointer(0);
    ids.clear();
    int nbPoints = cell->GetNumberOfPoints();
    ids.resize(nbPoints);
    if (!order.empty()) {
        for (int i = 0; i < nbPoints; ++i) {
            ids[i] = vtkIds[order[i]] + 1;
        }
    }
    else {
        for (int i = 0; i < nbPoints; ++i) {
            ids[i] = vtkIds[i] + 1;
        }
    }
}

}  // namespace


void FemVTKTools::importVTKMesh(vtkSmartPointer<vtkDataSet> dataset, FemMesh* mesh, float scale)
{
    const vtkIdType nPoints = dataset->GetNumberOfPoints();
    const vtkIdType nCells = dataset->GetNumberOfCells();
    Base::Console().Log("%d nodes/points and %d cells/elements found!\n", nPoints, nCells);
    Base::Console().Log("Build SMESH mesh out of the vtk mesh data.\n", nPoints, nCells);

    // Now fill the SMESH datastructure
    SMESH_Mesh* smesh = mesh->getSMesh();
    SMESHDS_Mesh* meshds = smesh->GetMeshDS();
    meshds->ClearMesh();

    for (vtkIdType i = 0; i < nPoints; i++) {
        double* p = dataset->GetPoint(i);
        meshds->AddNodeWithID(p[0] * scale, p[1] * scale, p[2] * scale, i + 1);
    }

    for (vtkIdType iCell = 0; iCell < nCells; iCell++) {
        vtkCell* cell = dataset->GetCell(iCell);
        std::vector<int> ids;
        fillMeshElementIds(cell, ids);
        switch (cell->GetCellType()) {
            // 1D edges
            case VTK_LINE:  // seg2
                meshds->AddEdgeWithID(ids[0], ids[1], iCell + 1);
                break;
            case VTK_QUADRATIC_EDGE:  // seg3
                meshds->AddEdgeWithID(ids[0], ids[1], ids[2], iCell + 1);
                break;
            // 2D faces
            case VTK_TRIANGLE:  // tria3
                meshds->AddFaceWithID(ids[0], ids[1], ids[2], iCell + 1);
                break;
            case VTK_QUADRATIC_TRIANGLE:  // tria6
                meshds->AddFaceWithID(ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], iCell + 1);
                break;
            case VTK_QUAD:  // quad4
                meshds->AddFaceWithID(ids[0], ids[1], ids[2], ids[3], iCell + 1);
                break;
            case VTK_QUADRATIC_QUAD:  // quad8
                meshds->AddFaceWithID(ids[0],
                                      ids[1],
                                      ids[2],
                                      ids[3],
                                      ids[4],
                                      ids[5],
                                      ids[6],
                                      ids[7],
                                      iCell + 1);
                break;
            // 3D volumes
            case VTK_TETRA:  // tetra4
                meshds->AddVolumeWithID(ids[0], ids[1], ids[2], ids[3], iCell + 1);
                break;
            case VTK_QUADRATIC_TETRA:  // tetra10
                meshds->AddVolumeWithID(ids[0],
                                        ids[1],
                                        ids[2],
                                        ids[3],
                                        ids[4],
                                        ids[5],
                                        ids[6],
                                        ids[7],
                                        ids[8],
                                        ids[9],
                                        iCell + 1);
                break;
            case VTK_HEXAHEDRON:  // hexa8
                meshds->AddVolumeWithID(ids[0],
                                        ids[1],
                                        ids[2],
                                        ids[3],
                                        ids[4],
                                        ids[5],
                                        ids[6],
                                        ids[7],
                                        iCell + 1);
                break;
            case VTK_QUADRATIC_HEXAHEDRON:  // hexa20
                meshds->AddVolumeWithID(ids[0],
                                        ids[1],
                                        ids[2],
                                        ids[3],
                                        ids[4],
                                        ids[5],
                                        ids[6],
                                        ids[7],
                                        ids[8],
                                        ids[9],
                                        ids[10],
                                        ids[11],
                                        ids[12],
                                        ids[13],
                                        ids[14],
                                        ids[15],
                                        ids[16],
                                        ids[17],
                                        ids[18],
                                        ids[19],
                                        iCell + 1);
                break;
            case VTK_WEDGE:  // penta6
                meshds->AddVolumeWithID(ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], iCell + 1);
                break;
            case VTK_QUADRATIC_WEDGE:  // penta15
                meshds->AddVolumeWithID(ids[0],
                                        ids[1],
                                        ids[2],
                                        ids[3],
                                        ids[4],
                                        ids[5],
                                        ids[6],
                                        ids[7],
                                        ids[8],
                                        ids[9],
                                        ids[10],
                                        ids[11],
                                        ids[12],
                                        ids[13],
                                        ids[14],
                                        iCell + 1);
                break;
            case VTK_PYRAMID:  // pyra5
                meshds->AddVolumeWithID(ids[0], ids[1], ids[2], ids[3], ids[4], iCell + 1);
                break;
            case VTK_QUADRATIC_PYRAMID:  // pyra13
                meshds->AddVolumeWithID(ids[0],
                                        ids[1],
                                        ids[2],
                                        ids[3],
                                        ids[4],
                                        ids[5],
                                        ids[6],
                                        ids[7],
                                        ids[8],
                                        ids[9],
                                        ids[10],
                                        ids[11],
                                        ids[12],
                                        iCell + 1);
                break;

            // not handled cases
            default: {
                Base::Console().Error(
                    "Only common 1D, 2D and 3D Cells are supported in VTK mesh import\n");
                break;
            }
        }
    }
}

FemMesh* FemVTKTools::readVTKMesh(const char* filename, FemMesh* mesh)
{
    Base::TimeElapsed Start;
    Base::Console().Log("Start: read FemMesh from VTK unstructuredGrid ======================\n");
    Base::FileInfo f(filename);

    if (f.hasExtension("vtu")) {
        vtkSmartPointer<vtkDataSet> dataset = readVTKFile<vtkXMLUnstructuredGridReader>(filename);
        if (!dataset.Get()) {
            Base::Console().Error("Failed to load file %s\n", filename);
            return nullptr;
        }
        importVTKMesh(dataset, mesh);
    }
    else if (f.hasExtension("pvtu")) {
        vtkSmartPointer<vtkDataSet> dataset = readVTKFile<vtkXMLPUnstructuredGridReader>(filename);
        if (!dataset.Get()) {
            Base::Console().Error("Failed to load file %s\n", filename);
            return nullptr;
        }
        importVTKMesh(dataset, mesh);
    }
    else if (f.hasExtension("vtk")) {
        vtkSmartPointer<vtkDataSet> dataset = readVTKFile<vtkDataSetReader>(filename);
        if (!dataset.Get()) {
            Base::Console().Error("Failed to load file %s\n", filename);
            return nullptr;
        }
        importVTKMesh(dataset, mesh);
    }
    else {
        Base::Console().Error("file name extension is not supported\n");
        return nullptr;
    }
    // Mesh should link to the part feature, in order to set up FemConstraint

    Base::Console().Log("    %f: Done \n",
                        Base::TimeElapsed::diffTimeF(Start, Base::TimeElapsed()));
    return mesh;
}

void exportFemMeshEdges(vtkSmartPointer<vtkCellArray>& elemArray,
                        std::vector<int>& types,
                        const SMDS_EdgeIteratorPtr& aEdgeIter)
{
    Base::Console().Log("  Start: VTK mesh builder edges.\n");

    while (aEdgeIter->more()) {
        const SMDS_MeshEdge* aEdge = aEdgeIter->next();
        // edge
        if (aEdge->GetEntityType() == SMDSEntity_Edge) {
            fillVtkArray<vtkLine>(elemArray, types, aEdge);
        }
        // quadratic edge
        else if (aEdge->GetEntityType() == SMDSEntity_Quad_Edge) {
            fillVtkArray<vtkQuadraticEdge>(elemArray, types, aEdge);
        }
        else {
            throw Base::TypeError("Edge not yet supported by FreeCAD's VTK mesh builder\n");
        }
    }

    Base::Console().Log("  End: VTK mesh builder edges.\n");
}

void exportFemMeshFaces(vtkSmartPointer<vtkCellArray>& elemArray,
                        std::vector<int>& types,
                        const SMDS_FaceIteratorPtr& aFaceIter)
{
    Base::Console().Log("  Start: VTK mesh builder faces.\n");

    while (aFaceIter->more()) {
        const SMDS_MeshFace* aFace = aFaceIter->next();
        // triangle
        if (aFace->GetEntityType() == SMDSEntity_Triangle) {
            fillVtkArray<vtkTriangle>(elemArray, types, aFace);
        }
        // quad
        else if (aFace->GetEntityType() == SMDSEntity_Quadrangle) {
            fillVtkArray<vtkQuad>(elemArray, types, aFace);
        }
        // quadratic triangle
        else if (aFace->GetEntityType() == SMDSEntity_Quad_Triangle) {
            fillVtkArray<vtkQuadraticTriangle>(elemArray, types, aFace);
        }
        // quadratic quad
        else if (aFace->GetEntityType() == SMDSEntity_Quad_Quadrangle) {
            fillVtkArray<vtkQuadraticQuad>(elemArray, types, aFace);
        }
        else {
            throw Base::TypeError("Face not yet supported by FreeCAD's VTK mesh builder\n");
        }
    }

    Base::Console().Log("  End: VTK mesh builder faces.\n");
}

void exportFemMeshCells(vtkSmartPointer<vtkCellArray>& elemArray,
                        std::vector<int>& types,
                        const SMDS_VolumeIteratorPtr& aVolIter)
{
    Base::Console().Log("  Start: VTK mesh builder volumes.\n");

    while (aVolIter->more()) {
        const SMDS_MeshVolume* aVol = aVolIter->next();

        if (aVol->GetEntityType() == SMDSEntity_Tetra) {  // tetra4
            fillVtkArray<vtkTetra>(elemArray, types, aVol);
        }
        else if (aVol->GetEntityType() == SMDSEntity_Pyramid) {  // pyra5
            fillVtkArray<vtkPyramid>(elemArray, types, aVol);
        }
        else if (aVol->GetEntityType() == SMDSEntity_Penta) {  // penta6
            fillVtkArray<vtkWedge>(elemArray, types, aVol);
        }
        else if (aVol->GetEntityType() == SMDSEntity_Hexa) {  // hexa8
            fillVtkArray<vtkHexahedron>(elemArray, types, aVol);
        }
        else if (aVol->GetEntityType() == SMDSEntity_Quad_Tetra) {  // tetra10
            fillVtkArray<vtkQuadraticTetra>(elemArray, types, aVol);
        }
        else if (aVol->GetEntityType() == SMDSEntity_Quad_Pyramid) {  // pyra13
            fillVtkArray<vtkQuadraticPyramid>(elemArray, types, aVol);
        }
        else if (aVol->GetEntityType() == SMDSEntity_Quad_Penta) {  // penta15
            fillVtkArray<vtkQuadraticWedge>(elemArray, types, aVol);
        }
        else if (aVol->GetEntityType() == SMDSEntity_Quad_Hexa) {  // hexa20
            fillVtkArray<vtkQuadraticHexahedron>(elemArray, types, aVol);
        }
        else {
            throw Base::TypeError("Volume not yet supported by FreeCAD's VTK mesh builder\n");
        }
    }

    Base::Console().Log("  End: VTK mesh builder volumes.\n");
}

void FemVTKTools::exportVTKMesh(const FemMesh* mesh,
                                vtkSmartPointer<vtkUnstructuredGrid> grid,
                                bool highest,
                                float scale)
{

    Base::Console().Log("Start: VTK mesh builder ======================\n");
    const SMESH_Mesh* smesh = mesh->getSMesh();
    const SMESHDS_Mesh* meshDS = smesh->GetMeshDS();

    // nodes
    Base::Console().Log("  Start: VTK mesh builder nodes.\n");

    vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
    SMDS_NodeIteratorPtr aNodeIter = meshDS->nodesIterator();

    while (aNodeIter->more()) {
        const SMDS_MeshNode* node = aNodeIter->next();  // why float, not double?
        double coords[3] = {double(node->X() * scale),
                            double(node->Y() * scale),
                            double(node->Z() * scale)};
        points->InsertPoint(node->GetID() - 1, coords);
        // memory is allocated by VTK points size for max node id, not for point count
        // if the SMESH mesh has gaps in node numbering, points without any element
        // assignment will be inserted in these point gaps too
        // this needs to be taken into account on node mapping when FreeCAD FEM results
        // are exported to vtk
    }
    grid->SetPoints(points);
    // nodes debugging
    const SMDS_MeshInfo& info = meshDS->GetMeshInfo();
    Base::Console().Log("    Size of nodes in SMESH grid: %i.\n", info.NbNodes());
    const vtkIdType nNodes = grid->GetNumberOfPoints();
    Base::Console().Log("    Size of nodes in VTK grid: %i.\n", nNodes);
    Base::Console().Log("  End: VTK mesh builder nodes.\n");

    vtkSmartPointer<vtkCellArray> elemArray = vtkSmartPointer<vtkCellArray>::New();
    std::vector<int> types;

    if (highest) {
        // try volumes
        SMDS_VolumeIteratorPtr aVolIter = meshDS->volumesIterator();
        exportFemMeshCells(elemArray, types, aVolIter);
        // try faces
        if (elemArray->GetNumberOfCells() == 0) {
            SMDS_FaceIteratorPtr aFaceIter = meshDS->facesIterator();
            exportFemMeshFaces(elemArray, types, aFaceIter);
        }
        // try edges
        if (elemArray->GetNumberOfCells() == 0) {
            SMDS_EdgeIteratorPtr aEdgeIter = meshDS->edgesIterator();
            exportFemMeshEdges(elemArray, types, aEdgeIter);
        }
    }
    else {
        // export all elements
        // edges
        SMDS_EdgeIteratorPtr aEdgeIter = meshDS->edgesIterator();
        exportFemMeshEdges(elemArray, types, aEdgeIter);
        // faces
        SMDS_FaceIteratorPtr aFaceIter = meshDS->facesIterator();
        exportFemMeshFaces(elemArray, types, aFaceIter);
        // volumes
        SMDS_VolumeIteratorPtr aVolIter = meshDS->volumesIterator();
        exportFemMeshCells(elemArray, types, aVolIter);
    }

    if (elemArray->GetNumberOfCells() > 0) {
        grid->SetCells(types.data(), elemArray);
    }

    Base::Console().Log("End: VTK mesh builder ======================\n");
}

void FemVTKTools::writeVTKMesh(const char* filename, const FemMesh* mesh, bool highest)
{

    Base::TimeElapsed Start;
    Base::Console().Log("Start: write FemMesh from VTK unstructuredGrid ======================\n");
    Base::FileInfo f(filename);

    vtkSmartPointer<vtkUnstructuredGrid> grid = vtkSmartPointer<vtkUnstructuredGrid>::New();
    exportVTKMesh(mesh, grid, highest);
    Base::Console().Log("Start: writing mesh data ======================\n");
    if (f.hasExtension("vtu")) {
        writeVTKFile<vtkXMLUnstructuredGridWriter>(filename, grid);
    }
    else if (f.hasExtension("vtk")) {
        writeVTKFile<vtkDataSetWriter>(filename, grid);
    }
    else {
        Base::Console().Error("file name extension is not supported to write VTK\n");
    }

    Base::Console().Log("    %f: Done \n",
                        Base::TimeElapsed::diffTimeF(Start, Base::TimeElapsed()));
}


App::DocumentObject* getObjectByType(const Base::Type type)
{
    App::Document* pcDoc = App::GetApplication().getActiveDocument();
    if (!pcDoc) {
        Base::Console().Message("No active document is found thus created\n");
        pcDoc = App::GetApplication().newDocument();
    }
    App::DocumentObject* obj = pcDoc->getActiveObject();

    if (obj->getTypeId() == type) {
        return obj;
    }
    if (obj->is<FemAnalysis>()) {
        std::vector<App::DocumentObject*> fem = (static_cast<FemAnalysis*>(obj))->Group.getValues();
        for (const auto& it : fem) {
            if (it->isDerivedFrom(type)) {
                return static_cast<App::DocumentObject*>(it);  // return the first of that type
            }
        }
    }
    return nullptr;
}


App::DocumentObject* createObjectByType(const Base::Type type)
{
    App::Document* pcDoc = App::GetApplication().getActiveDocument();
    if (!pcDoc) {
        Base::Console().Message("No active document is found thus created\n");
        pcDoc = App::GetApplication().newDocument();
    }
    App::DocumentObject* obj = pcDoc->getActiveObject();

    if (obj->is<FemAnalysis>()) {
        App::DocumentObject* newobj = pcDoc->addObject(type.getName());
        static_cast<FemAnalysis*>(obj)->addObject(newobj);
        return newobj;
    }
    else {
        return pcDoc->addObject(type.getName());  // create in the active document
    }
}


App::DocumentObject* FemVTKTools::readResult(const char* filename, App::DocumentObject* res)
{
    Base::TimeElapsed Start;
    Base::Console().Log(
        "Start: read FemResult with FemMesh from VTK file ======================\n");
    Base::FileInfo f(filename);

    vtkSmartPointer<vtkDataSet> ds;
    if (f.hasExtension("vtu")) {
        ds = readVTKFile<vtkXMLUnstructuredGridReader>(filename);
    }
    else if (f.hasExtension("vtk")) {
        ds = readVTKFile<vtkDataSetReader>(filename);
    }
    else {
        Base::Console().Error("file name extension is not supported\n");
    }

    App::Document* pcDoc = App::GetApplication().getActiveDocument();
    if (!pcDoc) {
        Base::Console().Message("No active document is found thus created\n");
        pcDoc = App::GetApplication().newDocument();
    }
    App::DocumentObject* obj = pcDoc->getActiveObject();

    vtkSmartPointer<vtkDataSet> dataset = ds;
    App::DocumentObject* result = nullptr;

    if (res) {
        Base::Console().Message(
            "FemResultObject pointer is NULL, trying to get the active object\n");
        if (obj->getTypeId() == Base::Type::fromName("Fem::FemResultObjectPython")) {
            result = obj;
        }
        else {
            Base::Console().Message("the active object is not the correct type, do nothing\n");
            return nullptr;
        }
    }

    App::DocumentObject* mesh = pcDoc->addObject("Fem::FemMeshObject", "ResultMesh");
    std::unique_ptr<FemMesh> fmesh(new FemMesh());
    importVTKMesh(dataset, fmesh.get());
    static_cast<PropertyFemMesh*>(mesh->getPropertyByName("FemMesh"))->setValuePtr(fmesh.release());

    if (result) {
        // PropertyLink is the property type to store DocumentObject pointer
        App::PropertyLink* link =
            dynamic_cast<App::PropertyLink*>(result->getPropertyByName("Mesh"));
        if (link) {
            link->setValue(mesh);
        }

        // vtkSmartPointer<vtkPointData> pd = dataset->GetPointData();
        importFreeCADResult(dataset, result);
    }

    pcDoc->recompute();
    Base::Console().Log("    %f: Done \n",
                        Base::TimeElapsed::diffTimeF(Start, Base::TimeElapsed()));
    Base::Console().Log("End: read FemResult with FemMesh from VTK file ======================\n");

    return result;
}


void FemVTKTools::writeResult(const char* filename, const App::DocumentObject* res)
{
    if (!res) {
        App::Document* pcDoc = App::GetApplication().getActiveDocument();
        if (!pcDoc) {
            Base::Console().Message("No active document is found thus do nothing and return\n");
            return;
        }
        res = pcDoc->getActiveObject();  // type checking is done by caller
    }
    if (!res) {
        Base::Console().Error("Result object pointer is invalid and it is not active object");
        return;
    }

    Base::TimeElapsed Start;
    Base::Console().Log("Start: write FemResult to VTK unstructuredGrid dataset =======\n");
    Base::FileInfo f(filename);

    // mesh
    vtkSmartPointer<vtkUnstructuredGrid> grid = vtkSmartPointer<vtkUnstructuredGrid>::New();
    App::DocumentObject* mesh =
        static_cast<App::PropertyLink*>(res->getPropertyByName("Mesh"))->getValue();
    const FemMesh& fmesh =
        static_cast<PropertyFemMesh*>(mesh->getPropertyByName("FemMesh"))->getValue();
    FemVTKTools::exportVTKMesh(&fmesh, grid);

    Base::Console().Log("    %f: vtk mesh builder finished\n",
                        Base::TimeElapsed::diffTimeF(Start, Base::TimeElapsed()));

    // result
    FemVTKTools::exportFreeCADResult(res, grid);

    if (f.hasExtension("vtu")) {
        writeVTKFile<vtkXMLUnstructuredGridWriter>(filename, grid);
    }
    else if (f.hasExtension("vtk")) {
        writeVTKFile<vtkDataSetWriter>(filename, grid);
    }
    else {
        Base::Console().Error("file name extension is not supported to write VTK\n");
    }

    Base::Console().Log("    %f: writing result object to vtk finished\n",
                        Base::TimeElapsed::diffTimeF(Start, Base::TimeElapsed()));
    Base::Console().Log("End: write FemResult to VTK unstructuredGrid dataset =======\n");
}


std::map<std::string, std::string> _getFreeCADMechResultVectorProperties()
{
    // see src/Mod/Fem/femobjects/_FemResultMechanical
    // App::PropertyVectorList will be a list of vectors in vtk
    std::map<std::string, std::string> resFCVecProp;
    resFCVecProp["DisplacementVectors"] = "Displacement";
    // the following three are filled only if there is a reinforced mat object
    // https://forum.freecad.org/viewtopic.php?f=18&t=33106&start=70#p296317
    // https://forum.freecad.org/viewtopic.php?f=18&t=33106&p=416006#p412800
    resFCVecProp["PS1Vector"] = "Major Principal Stress Vector";
    resFCVecProp["PS2Vector"] = "Intermediate Principal Stress Vector";
    resFCVecProp["PS3Vector"] = "Minor Principal Stress Vector";
    resFCVecProp["HeatFlux"] = "Heat Flux";

    return resFCVecProp;
}

// see https://forum.freecad.org/viewtopic.php?f=18&t=33106&start=30#p277434 for further
// information regarding names etc...
// some scalar list are not needed on VTK file export but they are needed for internal VTK pipeline
// TODO some filter to only export the needed values to VTK file but have all
// in FreeCAD VTK pipeline
std::map<std::string, std::string> _getFreeCADMechResultScalarProperties()
{
    // see src/Mod/Fem/femobjects/result_mechanical.py
    // App::PropertyFloatList will be a list of scalars in vtk
    std::map<std::string, std::string> resFCScalProp;
    resFCScalProp["DisplacementLengths"] =
        "Displacement Magnitude";  // can be plotted in Paraview as THE DISPLACEMENT MAGNITUDE
    resFCScalProp["MaxShear"] = "Tresca Stress";
    resFCScalProp["NodeStressXX"] = "Stress xx component";
    resFCScalProp["NodeStressYY"] = "Stress yy component";
    resFCScalProp["NodeStressZZ"] = "Stress zz component";
    resFCScalProp["NodeStressXY"] = "Stress xy component";
    resFCScalProp["NodeStressXZ"] = "Stress xz component";
    resFCScalProp["NodeStressYZ"] = "Stress yz component";
    resFCScalProp["NodeStrainXX"] = "Strain xx component";
    resFCScalProp["NodeStrainYY"] = "Strain yy component";
    resFCScalProp["NodeStrainZZ"] = "Strain zz component";
    resFCScalProp["NodeStrainXY"] = "Strain xy component";
    resFCScalProp["NodeStrainXZ"] = "Strain xz component";
    resFCScalProp["NodeStrainYZ"] = "Strain yz component";
    resFCScalProp["Peeq"] = "Equivalent Plastic Strain";
    resFCScalProp["CriticalStrainRatio"] = "Critical Strain Ratio";

    // the following three are filled in all cases
    // https://forum.freecad.org/viewtopic.php?f=18&t=33106&start=70#p296317
    // it might be these can be generated in paraview from stress tensor values as
    // THE MAJOR PRINCIPAL STRESS MAGNITUDE, THE INTERMEDIATE PRINCIPAL STRESS MAGNITUDE,
    // THE MINOR PRINCIPAL STRESS MAGNITUDE
    // but I do not know how (Bernd), for some help see paraview tutorial on FreeCAD wiki
    // thus TODO they might not be exported to external file format (first I need to know
    // how to generate them in paraview)
    // but there are needed anyway because the pipeline in FreeCAD needs the principal stress values
    // https://forum.freecad.org/viewtopic.php?f=18&t=33106&p=416006#p412800
    resFCScalProp["PrincipalMax"] = "Major Principal Stress";  // can be plotted in Paraview as THE
                                                               // MAJOR PRINCIPAL STRESS MAGNITUDE
    resFCScalProp["PrincipalMed"] =
        "Intermediate Principal Stress";  // can be plotted in Paraview as THE INTERMEDIATE
                                          // PRINCIPAL STRESS MAGNITUDE
    resFCScalProp["PrincipalMin"] = "Minor Principal Stress";  // can be plotted in Paraview as THE
                                                               // MINOR PRINCIPAL STRESS MAGNITUDE
    resFCScalProp["vonMises"] = "von Mises Stress";
    resFCScalProp["Temperature"] = "Temperature";
    resFCScalProp["MohrCoulomb"] = "MohrCoulomb";
    resFCScalProp["ReinforcementRatio_x"] = "ReinforcementRatio_x";
    resFCScalProp["ReinforcementRatio_y"] = "ReinforcementRatio_y";
    resFCScalProp["ReinforcementRatio_z"] = "ReinforcementRatio_z";

    resFCScalProp["UserDefined"] = "UserDefinedMyName";  // this is empty or am I wrong ?!
    resFCScalProp["MassFlowRate"] = "Mass Flow Rate";
    resFCScalProp["NetworkPressure"] = "Network Pressure";

    return resFCScalProp;
}


void FemVTKTools::importFreeCADResult(vtkSmartPointer<vtkDataSet> dataset,
                                      App::DocumentObject* result)
{
    Base::Console().Log("Start: import vtk result file data into a FreeCAD result object.\n");

    std::map<std::string, std::string> vectors = _getFreeCADMechResultVectorProperties();
    std::map<std::string, std::string> scalars = _getFreeCADMechResultScalarProperties();

    double ts = 0.0;  // t=0.0 for static simulation
    static_cast<App::PropertyFloat*>(result->getPropertyByName("Time"))->setValue(ts);

    vtkSmartPointer<vtkPointData> pd = dataset->GetPointData();
    if (pd->GetNumberOfArrays() == 0) {
        Base::Console().Error("No point data array is found in vtk data set, do nothing\n");
        // if pointData is empty, data may be in cellDate,
        // cellData -> pointData interpolation is possible in VTK
        return;
    }

    // NodeNumbers
    const vtkIdType nPoints = dataset->GetNumberOfPoints();
    std::vector<long> nodeIds(nPoints);
    for (vtkIdType i = 0; i < nPoints; ++i) {
        nodeIds[i] = i + 1;
    }
    static_cast<App::PropertyIntegerList*>(result->getPropertyByName("NodeNumbers"))
        ->setValues(nodeIds);
    Base::Console().Log("    NodeNumbers have been filled with values.\n");

    // vectors
    for (const auto& it : vectors) {
        int dim = 3;  // Fixme: currently 3D only, here we could run into trouble,
                      //        FreeCAD only supports dim 3D, I do not know about VTK
        vtkDataArray* vector_field = vtkDataArray::SafeDownCast(pd->GetArray(it.second.c_str()));
        if (vector_field && vector_field->GetNumberOfComponents() == dim) {
            App::PropertyVectorList* vector_list =
                static_cast<App::PropertyVectorList*>(result->getPropertyByName(it.first.c_str()));
            if (vector_list) {
                std::vector<Base::Vector3d> vec(nPoints);
                for (vtkIdType i = 0; i < nPoints; ++i) {
                    double* p = vector_field->GetTuple(
                        i);  // both vtkFloatArray and vtkDoubleArray return double* for GetTuple(i)
                    vec[i] = (Base::Vector3d(p[0], p[1], p[2]));
                }
                // PropertyVectorList will not show up in PropertyEditor
                vector_list->setValues(vec);
                Base::Console().Log("    A PropertyVectorList has been filled with values: %s\n",
                                    it.first.c_str());
            }
            else {
                Base::Console().Error("static_cast<App::PropertyVectorList*>((result->"
                                      "getPropertyByName(\"%s\")) failed.\n",
                                      it.first.c_str());
                continue;
            }
        }
        else {
            Base::Console().Message("    PropertyVectorList NOT found in vkt file data: %s\n",
                                    it.first.c_str());
        }
    }

    // scalars
    for (const auto& scalar : scalars) {
        vtkDataArray* vec = vtkDataArray::SafeDownCast(pd->GetArray(scalar.second.c_str()));
        if (nPoints && vec && vec->GetNumberOfComponents() == 1) {
            App::PropertyFloatList* field = static_cast<App::PropertyFloatList*>(
                result->getPropertyByName(scalar.first.c_str()));
            if (!field) {
                Base::Console().Error("static_cast<App::PropertyFloatList*>((result->"
                                      "getPropertyByName(\"%s\")) failed.\n",
                                      scalar.first.c_str());
                continue;
            }

            double vmin = 1.0e100, vmax = -1.0e100;
            std::vector<double> values(nPoints, 0.0);
            for (vtkIdType i = 0; i < vec->GetNumberOfTuples(); i++) {
                double v = *(vec->GetTuple(i));
                values[i] = v;
                if (v > vmax) {
                    vmax = v;
                }
                if (v < vmin) {
                    vmin = v;
                }
            }
            field->setValues(values);
            Base::Console().Log("    A PropertyFloatList has been filled with vales: %s\n",
                                scalar.first.c_str());
        }
        else {
            Base::Console().Message("    PropertyFloatList NOT found in vkt file data %s\n",
                                    scalar.first.c_str());
        }
    }

    // stats
    // stats are added by importVTKResults

    Base::Console().Log("End: import vtk result file data into a FreeCAD result object.\n");
}


void FemVTKTools::exportFreeCADResult(const App::DocumentObject* result,
                                      vtkSmartPointer<vtkDataSet> grid)
{
    Base::Console().Log("Start: Create VTK result data from FreeCAD result data.\n");

    std::map<std::string, std::string> vectors = _getFreeCADMechResultVectorProperties();
    std::map<std::string, std::string> scalars = _getFreeCADMechResultScalarProperties();

    const Fem::FemResultObject* res = static_cast<const Fem::FemResultObject*>(result);
    const vtkIdType nPoints = grid->GetNumberOfPoints();

    // we need the corresponding mesh to get the correct id for the result data
    // (when the freecad smesh mesh has gaps in the points
    // vtk has more points. Vtk does not support point gaps, thus the gaps are
    // filled with points. Then the mapping must be correct)
    App::DocumentObject* meshObj = res->Mesh.getValue();
    if (!meshObj || !meshObj->isDerivedFrom<FemMeshObject>()) {
        Base::Console().Error("Result object does not correctly link to mesh");
        return;
    }
    const SMESH_Mesh* smesh = static_cast<FemMeshObject*>(meshObj)->FemMesh.getValue().getSMesh();
    const SMESHDS_Mesh* meshDS = smesh->GetMeshDS();

    // all result object meshes are in mm therefore for e.g. length outputs like
    // displacement we must divide by 1000
    double factor = 1.0;

    // vectors
    for (const auto& it : vectors) {
        const int dim =
            3;  // Fixme, detect dim, but FreeCAD PropertyVectorList ATM only has DIM of 3
        App::PropertyVectorList* field = nullptr;
        if (res->getPropertyByName(it.first.c_str())) {
            field = static_cast<App::PropertyVectorList*>(res->getPropertyByName(it.first.c_str()));
        }
        else {
            Base::Console().Error("    PropertyVectorList not found: %s\n", it.first.c_str());
        }

        if (field && field->getSize() > 0) {
            const std::vector<Base::Vector3d>& vel = field->getValues();
            vtkSmartPointer<vtkDoubleArray> data = vtkSmartPointer<vtkDoubleArray>::New();
            data->SetNumberOfComponents(dim);
            data->SetNumberOfTuples(nPoints);
            data->SetName(it.second.c_str());

            // we need to set values for the unused points.
            // TODO: ensure that the result bar does not include the used 0 if it is not
            // part of the result (e.g. does the result bar show 0 as smallest value?)
            if (nPoints != field->getSize()) {
                double tuple[] = {0, 0, 0};
                for (vtkIdType i = 0; i < nPoints; ++i) {
                    data->SetTuple(i, tuple);
                }
            }

            if (it.first.compare("DisplacementVectors") == 0) {
                factor = 0.001;  // to get meter
            }
            else {
                factor = 1.0;
            }

            SMDS_NodeIteratorPtr aNodeIter = meshDS->nodesIterator();
            for (const auto& jt : vel) {
                const SMDS_MeshNode* node = aNodeIter->next();
                double tuple[] = {jt.x * factor, jt.y * factor, jt.z * factor};
                data->SetTuple(node->GetID() - 1, tuple);
            }
            grid->GetPointData()->AddArray(data);
            Base::Console().Log(
                "    The PropertyVectorList %s was exported to VTK vector list: %s\n",
                it.first.c_str(),
                it.second.c_str());
        }
        else if (field) {
            Base::Console().Log("    PropertyVectorList NOT exported to vtk: %s size is: %i\n",
                                it.first.c_str(),
                                field->getSize());
        }
    }

    // scalars
    for (const auto& scalar : scalars) {
        App::PropertyFloatList* field = nullptr;
        if (res->getPropertyByName(scalar.first.c_str())) {
            field =
                static_cast<App::PropertyFloatList*>(res->getPropertyByName(scalar.first.c_str()));
        }
        else {
            Base::Console().Error("PropertyFloatList %s not found \n", scalar.first.c_str());
        }

        if (field && field->getSize() > 0) {
            const std::vector<double>& vec = field->getValues();
            vtkSmartPointer<vtkDoubleArray> data = vtkSmartPointer<vtkDoubleArray>::New();
            data->SetNumberOfValues(nPoints);
            data->SetName(scalar.second.c_str());

            // we need to set values for the unused points.
            // TODO: ensure that the result bar does not include the used 0 if it is not part
            // of the result (e.g. does the result bar show 0 as smallest value?)
            if (nPoints != field->getSize()) {
                for (vtkIdType i = 0; i < nPoints; ++i) {
                    data->SetValue(i, 0);
                }
            }

            if ((scalar.first.compare("MaxShear") == 0)
                || (scalar.first.compare("NodeStressXX") == 0)
                || (scalar.first.compare("NodeStressXY") == 0)
                || (scalar.first.compare("NodeStressXZ") == 0)
                || (scalar.first.compare("NodeStressYY") == 0)
                || (scalar.first.compare("NodeStressYZ") == 0)
                || (scalar.first.compare("NodeStressZZ") == 0)
                || (scalar.first.compare("PrincipalMax") == 0)
                || (scalar.first.compare("PrincipalMed") == 0)
                || (scalar.first.compare("PrincipalMin") == 0)
                || (scalar.first.compare("vonMises") == 0)
                || (scalar.first.compare("NetworkPressure") == 0)) {
                factor = 1e6;  // to get Pascal
            }
            else if (scalar.first.compare("DisplacementLengths") == 0) {
                factor = 0.001;  // to get meter
            }
            else {
                factor = 1.0;
            }

            SMDS_NodeIteratorPtr aNodeIter = meshDS->nodesIterator();
            for (double i : vec) {
                const SMDS_MeshNode* node = aNodeIter->next();
                // for the MassFlowRate the last vec entries can be a nullptr, thus check this
                if (node) {
                    data->SetValue(node->GetID() - 1, i * factor);
                }
            }

            grid->GetPointData()->AddArray(data);
            Base::Console().Log(
                "    The PropertyFloatList %s was exported to VTK scalar list: %s\n",
                scalar.first.c_str(),
                scalar.second.c_str());
        }
        else if (field) {
            Base::Console().Log("    PropertyFloatList NOT exported to vtk: %s size is: %i\n",
                                scalar.first.c_str(),
                                field->getSize());
        }
    }

    Base::Console().Log("End: Create VTK result data from FreeCAD result data.\n");
}

}  // namespace Fem
