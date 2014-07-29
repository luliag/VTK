/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkXdmf3Reader.cxx
  Language:  C++
  Date:      $Date$
  Version:   $Revision$

  Copyright (c) 1993-2002 Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkXdmf3Reader.h"

#include "vtksys/SystemTools.hxx"
#include "vtkDataObjectTypes.h"
#include "vtkDataSetAttributes.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkImageData.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkMultiPieceDataSet.h"
#include "vtkMultiProcessController.h"
#include "vtkMutableDirectedGraph.h"
#include "vtkObjectFactory.h"
#include "vtkRectilinearGrid.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStringArray.h"
#include "vtkStructuredGrid.h"
#include "vtkTimerLog.h"
#include "vtkUniformGrid.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnstructuredGrid.h"
#include "vtkXdmf3ArrayKeeper.h"
#include "vtkXdmf3ArraySelection.h"
#include "vtkXdmf3DataSet.h"

#include "XdmfAttribute.hpp"
#include "XdmfAttributeCenter.hpp"
#include "XdmfCurvilinearGrid.hpp"
#include "XdmfDomain.hpp"
#include "XdmfGraph.hpp"
#include "XdmfGridCollection.hpp"
#include "XdmfGridCollectionType.hpp"
#include "XdmfReader.hpp"
#include "XdmfRectilinearGrid.hpp"
#include "XdmfRegularGrid.hpp"
#include "XdmfSet.hpp"
#include "XdmfTime.hpp"
#include "XdmfUnstructuredGrid.hpp"
#include "XdmfVisitor.hpp"

#include <set>
#include <fstream>

//TODO: implement fast and approximate CanReadFile
//TODO: read from buffer, allowing for xincludes
//TODO: strided access to structured data
//TODO: when too many grids for SIL, allow selection of top level grids
//TODO: break structured data into pieces
//TODO: make domains entirely optional and selectable

// As soon as num-grids (sub-grids and all) grows beyond this number, we assume
// that the grids are too numerous for the user to select individually and
// hence only the top-level grids are made accessible.
#define MAX_COLLECTABLE_NUMBER_OF_GRIDS 1000

//=============================================================================
class vtkXdmf3Reader_SILBuilder
{
public:
  vtkStringArray* NamesArray;
  vtkUnsignedCharArray* CrossEdgesArray;
  vtkMutableDirectedGraph* SIL;
  vtkIdType RootVertex;
  vtkIdType BlocksRoot;
  vtkIdType HierarchyRoot;
  vtkIdType VertexCount;

  // Description:
  // Initializes the data-structures.
  void Initialize()
  {
    if (this->SIL)
      {
      this->SIL->Delete();
      }
    this->SIL = vtkMutableDirectedGraph::New();
    this->SIL->Initialize();

    if (this->NamesArray)
      {
      this->NamesArray->Delete();
      }
    this->NamesArray = vtkStringArray::New();
    this->NamesArray->SetName("Names");
    this->SIL->GetVertexData()->AddArray(this->NamesArray);

    if (this->CrossEdgesArray)
      {
      this->CrossEdgesArray->Delete();
      }

    this->CrossEdgesArray = vtkUnsignedCharArray::New();
    this->CrossEdgesArray->SetName("CrossEdges");
    this->SIL->GetEdgeData()->AddArray(this->CrossEdgesArray);

    this->RootVertex = this->AddVertex("SIL");
    this->BlocksRoot = this->AddVertex("Blocks");
    this->HierarchyRoot = this->AddVertex("Hierarchy");
    this->AddChildEdge(RootVertex, BlocksRoot);
    this->AddChildEdge(RootVertex, HierarchyRoot);

    this->VertexCount = 0;
  }

  // Description:
  // Add vertex, child-edge or cross-edge to the graph.
  vtkIdType AddVertex(const char* name)
  {
    this->VertexCount++;
    vtkIdType vertex = this->SIL->AddVertex();
    this->NamesArray->InsertValue(vertex, name);
    return vertex;
  }
  vtkIdType AddChildEdge(vtkIdType parent, vtkIdType child)
  {
    vtkIdType id = this->SIL->AddEdge(parent, child).Id;
    this->CrossEdgesArray->InsertValue(id, 0);
    return id;
  }
  vtkIdType AddCrossEdge(vtkIdType src, vtkIdType dst)
  {
    vtkIdType id = this->SIL->AddEdge(src, dst).Id;
    this->CrossEdgesArray->InsertValue(id, 1);
    return id;
  }

  // Description:
  // Returns the vertex id for the root vertex.
  vtkIdType GetRootVertex()
  {
    return this->RootVertex;
  }
  vtkIdType GetBlocksRoot()
  {
    return this->BlocksRoot;
  }
  vtkIdType GetHierarchyRoot()
  {
    return this->HierarchyRoot;
  }

  bool IsMaxedOut()
  {
    return (this->VertexCount >= MAX_COLLECTABLE_NUMBER_OF_GRIDS);
  }

  vtkXdmf3Reader_SILBuilder()
  {
    this->SIL = NULL;
    this->NamesArray = NULL;
    this->CrossEdgesArray = NULL;
    this->RootVertex = -1;
    this->BlocksRoot = -1;
    this->HierarchyRoot = -1;
    this->VertexCount = 0;
  }
  ~vtkXdmf3Reader_SILBuilder()
  {
    if (this->SIL)
      {
      this->SIL->Delete();
      }
    if (this->NamesArray)
      {
      this->NamesArray->Delete();
      }
    if (this->CrossEdgesArray)
      {
      this->CrossEdgesArray->Delete();
      }
  }
};

//=============================================================================
class vtkXdmfVisitor_Translator
{
  //Traverses the XDMF structure to translate into native VTK representations
  //of the contents. Afterward, we can obtain meta information such as
  //the vtk type of the data, the available timesteps, a serializable vtkGraph
  //of the hierarchy (SIL), block lists, and populate the vtk data set.

public:

  //////////////////////
  //Setup
  //////////////////////
  static shared_ptr<vtkXdmfVisitor_Translator> New(
      vtkXdmf3Reader_SILBuilder *sb,
      vtkXdmf3ArraySelection* f,
      vtkXdmf3ArraySelection* ce,
      vtkXdmf3ArraySelection* pn,
      vtkXdmf3ArraySelection* gc,
      vtkXdmf3ArraySelection* sc,
      unsigned int processor,
      unsigned int nprocessors
      )
  {
    shared_ptr<vtkXdmfVisitor_Translator> p(new vtkXdmfVisitor_Translator());
    p->SILBuilder = sb;
    p->FieldArrays = f;
    p->CellArrays = ce;
    p->PointArrays = pn;
    p->GridsCache = gc;
    p->SetsCache = sc;
    p->MaxDepth = 0;
    p->Rank = processor;
    p->NumProcs = nprocessors;
    return p;
  }

  //////////////////////
  //Build
  //////////////////////
  // recursively inspect XDMF data hierarchy to determine
  // times that we can provide data at
  // name of arrays to select from
  // name and hierarchical relationship of blocks to select from
  void InspectXDMF(shared_ptr<XdmfItem> item, vtkIdType parentVertex, unsigned int depth=0)
  {
    assert(this->SILBuilder);
    assert(this->FieldArrays);
    assert(this->PointArrays);
    assert(this->CellArrays);
    assert(this->GridsCache);
    assert(this->SetsCache);

    if (this->TooDeep(depth))
      {
      return;
      }

    this->InspectArrays(item);
    this->InspectTime(item);

    shared_ptr<XdmfDomain> coll = shared_dynamic_cast<XdmfDomain>(item);
    if (!coll)
      {
      if (this->SILBuilder->IsMaxedOut())
        {
        return;
        }

      shared_ptr<XdmfGrid> grid = shared_dynamic_cast<XdmfGrid>(item);
      if (grid)
        {
        //atomic dataset
        vtkIdType parent = parentVertex;
        if (parentVertex == -1)
          {
          parent = this->SILBuilder->GetHierarchyRoot();
          }
        unsigned int nSets = grid->getNumberSets();
        std::string name = grid->getName();
        if (name.length() != 0 && (nSets > 0 || parentVertex != -1))
          {
          std::string uName = this->UniqueName(name, true);
          grid->setName(uName);
          this->AddNamedBlock(parent, name, uName);
          for (unsigned int s = 0; s < nSets; s++)
            {
            shared_ptr<XdmfSet> set = grid->getSet(s);
            std::string sName = set->getName();
            std::string usName = this->UniqueName(sName, false);
            set->setName(usName);
            this->AddNamedSet(usName);
            }
          }
        return;
        }

      shared_ptr<XdmfGraph> graph = shared_dynamic_cast<XdmfGraph>(item);
      if (graph)
        {
        std::string name = graph->getName();
        if (name.length() != 0 && parentVertex != -1)
          {
          std::string uName = this->UniqueName(name, true);
          graph->setName(uName);
          this->AddNamedBlock(parentVertex, name, uName);
          }
        return;
        }

      cerr << "Found unknown Xdmf data type" << endl;
      return;
      }
    else
      {
      //four cases: domain, temporal, spatial or hierarchical
      shared_ptr<XdmfGridCollection> asGC = shared_dynamic_cast<XdmfGridCollection>(item);
      bool isDomain = asGC?false:true;

      bool isTemporal = false;
      if (asGC && asGC->getType() == XdmfGridCollectionType::Temporal())
        {
        isTemporal = true;
        }

      vtkIdType silVertex = parentVertex;
      if (!isTemporal && !isDomain)
        {
        std::string name = asGC->getName();
        if (name.length() != 0 && !this->SILBuilder->IsMaxedOut())
          {
          silVertex = this->SILBuilder->AddVertex(name.c_str());
          vtkIdType parent = parentVertex;
          if (parentVertex == -1)
            {
            //topmost entry, we are the root
            parent = this->SILBuilder->GetHierarchyRoot();
            }
          this->SILBuilder->AddChildEdge(parent, silVertex);
          }
        }

      unsigned int nGridCollections = coll->getNumberGridCollections();
      for (unsigned int i = 0; i < nGridCollections; i++)
        {
        if (isDomain && !this->ShouldRead(i, nGridCollections))
          {
          continue;
          }
        shared_ptr<XdmfGrid> child = coll->getGridCollection(i);
        this->InspectXDMF(child, silVertex, depth+1);
        }
      unsigned int nUnstructuredGrids = coll->getNumberUnstructuredGrids();
      for (unsigned int i = 0; i < nUnstructuredGrids; i++)
        {
        shared_ptr<XdmfGrid> child = coll->getUnstructuredGrid(i);
        this->InspectXDMF(child, silVertex, depth+1);
        }
      unsigned int nRectilinearGrids = coll->getNumberRectilinearGrids();
      for (unsigned int i = 0; i < nRectilinearGrids; i++)
        {
        shared_ptr<XdmfGrid> child = coll->getRectilinearGrid(i);
        this->InspectXDMF(child, silVertex, depth+1);
        }
      unsigned int nCurvilinearGrids= coll->getNumberCurvilinearGrids();
      for (unsigned int i = 0; i < nCurvilinearGrids; i++)
        {
        shared_ptr<XdmfGrid> child = coll->getCurvilinearGrid(i);
        this->InspectXDMF(child, silVertex, depth+1);
        }
      unsigned int nRegularGrids = coll->getNumberRegularGrids();
      for (unsigned int i = 0; i < nRegularGrids; i++)
        {
        shared_ptr<XdmfGrid> child = coll->getRegularGrid(i);
        this->InspectXDMF(child, silVertex, depth+1);
        }
      unsigned int nGraphs = coll->getNumberGraphs();
      for (unsigned int i = 0; i < nGraphs; i++)
        {
        shared_ptr<XdmfGraph> child = coll->getGraph(i);
        this->InspectXDMF(child, silVertex, depth+1);
        }
      }
  }

  // called to make sure overflown SIL doesn't give nonsensical results
  void ClearGridsIfNeeded(shared_ptr<XdmfItem> domain)
  {
    if (this->SILBuilder->IsMaxedOut())
      {
      //too numerous to be of use to user for manual selection, so clear out
      this->GridsCache->clear();
      this->SetsCache->clear();
      this->SILBuilder->Initialize();
      this->MaxDepth = 4;
      this->InspectXDMF(domain, -1);
      }
  }

  //////////////////////
  // Use
  //////////////////////
  //return the list of times that the xdmf file can provide data at
  //only valid after InspectXDMF
  std::set<double> getTimes()
  {
    return times;
  }

  ~vtkXdmfVisitor_Translator() {}

private:
  vtkXdmfVisitor_Translator() {}

  void InspectArrays(shared_ptr<XdmfItem> item)
  {
    shared_ptr<XdmfGrid> grid = shared_dynamic_cast<XdmfGrid>(item);
    if (grid)
      {
      unsigned int numAttributes = grid->getNumberAttributes();
      for (unsigned int cc=0; cc < numAttributes; cc++)
        {
        shared_ptr<XdmfAttribute> xmfAttribute = grid->getAttribute(cc);
        std::string attrName = xmfAttribute->getName();
        if (attrName.length() == 0)
          {
          cerr << "Skipping unnamed array." << endl;
          continue;
          }
        shared_ptr<const XdmfAttributeCenter> attrCenter = xmfAttribute->getCenter();
        if (attrCenter == XdmfAttributeCenter::Grid())
          {
          if (!this->FieldArrays->HasArray(attrName.c_str()))
            {
            this->FieldArrays->AddArray(attrName.c_str());
            }
          }
        else if (attrCenter == XdmfAttributeCenter::Cell())
          {
          if (!this->CellArrays->HasArray(attrName.c_str()))
            {
            this->CellArrays->AddArray(attrName.c_str());
            }
          }
        else if (attrCenter == XdmfAttributeCenter::Node())
          {
          if (!this->PointArrays->HasArray(attrName.c_str()))
            {
            this->PointArrays->AddArray(attrName.c_str());
            }
          }
        else
          {
          cerr << "Skipping " << attrName << " unrecognized association" << endl;
          continue;
          }
        }
      }
    else
      {
      shared_ptr<XdmfGraph> graph = shared_dynamic_cast<XdmfGraph>(item);
      if (graph)
        {
        unsigned int numAttributes = graph->getNumberAttributes();
        for (unsigned int cc=0; cc < numAttributes; cc++)
          {
          shared_ptr<XdmfAttribute> xmfAttribute = graph->getAttribute(cc);
          std::string attrName = xmfAttribute->getName();
          if (attrName.length() == 0)
            {
            cerr << "Skipping unnamed array." << endl;
            continue;
            }
          shared_ptr<const XdmfAttributeCenter> attrCenter = xmfAttribute->getCenter();
          if (attrCenter == XdmfAttributeCenter::Grid())
            {
            if (!this->FieldArrays->HasArray(attrName.c_str()))
              {
              this->FieldArrays->AddArray(attrName.c_str());
              }
            }
          else if (attrCenter == XdmfAttributeCenter::Edge())
            {
            if (!this->CellArrays->HasArray(attrName.c_str()))
              {
              this->CellArrays->AddArray(attrName.c_str());
              }
            }
          else if (attrCenter == XdmfAttributeCenter::Node())
            {
            if (!this->PointArrays->HasArray(attrName.c_str()))
              {
              this->PointArrays->AddArray(attrName.c_str());
              }
            }
          else
            {
            cerr << "Skipping " << attrName << " unrecognized association" << endl;
            continue;
            }
          }
        }
      }
  }

  //helper for InspectXDMF
  bool TooDeep(unsigned int depth)
  {
    if (this->MaxDepth != 0 && depth >= this->MaxDepth)
      {
      return true;
      }
    return false;
  }

  //helper for InspectXDMF
  std::string UniqueName(std::string name, bool ForGrid)
  {
    std::string gridName = name;
    unsigned int count=1;

    vtkXdmf3ArraySelection* cache = (ForGrid?this->GridsCache:this->SetsCache);
    while (cache->HasArray(gridName.c_str()))
      {
      vtksys_ios::ostringstream str;
      str << name << "[" << count << "]";
      gridName = str.str();
      count++;
      }
    return gridName;
  }

  //helper for InspectXDMF
  void AddNamedBlock(vtkIdType parentVertex, std::string originalName, std::string uniqueName)
  {
    this->GridsCache->AddArray(uniqueName.c_str());

    vtkIdType silVertex = this->SILBuilder->AddVertex(uniqueName.c_str());
    this->SILBuilder->AddChildEdge(this->SILBuilder->GetBlocksRoot(), silVertex);

    vtkIdType hierarchyVertex = this->SILBuilder->AddVertex(originalName.c_str());
    this->SILBuilder->AddChildEdge(parentVertex, hierarchyVertex);
    this->SILBuilder->AddCrossEdge(hierarchyVertex, silVertex);
  }

  //helper for InspectXDMF
  void AddNamedSet(std::string uniqueName)
  {
    this->SetsCache->AddArray(uniqueName.c_str());
  }

  //records times that xdmf grids supply data at
  //if timespecs are only implied we add them to make things simpler later on
  void InspectTime(shared_ptr<XdmfItem> item)
  {
    shared_ptr<XdmfGridCollection> gc = shared_dynamic_cast<XdmfGridCollection>(item);
    if (gc && gc->getType() == XdmfGridCollectionType::Temporal())
      {
      unsigned int cnt = 0;
      unsigned int nGridCollections = gc->getNumberGridCollections();
      for (unsigned int i = 0; i < nGridCollections; i++)
        {
        shared_ptr<XdmfGrid> child = gc->getGridCollection(i);
        this->GetSetTime(child, cnt);
        }
      unsigned int nUnstructuredGrids = gc->getNumberUnstructuredGrids();
      for (unsigned int i = 0; i < nUnstructuredGrids; i++)
        {
        shared_ptr<XdmfGrid> child = gc->getUnstructuredGrid(i);
        this->GetSetTime(child, cnt);
        }
      unsigned int nRectilinearGrids = gc->getNumberRectilinearGrids();
      for (unsigned int i = 0; i < nRectilinearGrids; i++)
        {
        shared_ptr<XdmfGrid> child = gc->getRectilinearGrid(i);
        this->GetSetTime(child, cnt);
        }
      unsigned int nCurvilinearGrids= gc->getNumberCurvilinearGrids();
      for (unsigned int i = 0; i < nCurvilinearGrids; i++)
        {
        shared_ptr<XdmfGrid> child = gc->getCurvilinearGrid(i);
        this->GetSetTime(child, cnt);
        }
      unsigned int nRegularGrids = gc->getNumberRegularGrids();
      for (unsigned int i = 0; i < nRegularGrids; i++)
        {
        shared_ptr<XdmfGrid> child = gc->getRegularGrid(i);
        this->GetSetTime(child, cnt);
        }
      unsigned int nGraphs = gc->getNumberGraphs();
      for (unsigned int i = 0; i < nGraphs; i++)
        {
        shared_ptr<XdmfGraph> child = gc->getGraph(i);
        this->GetSetTime(child, cnt);
        }
      }
  }

  //helper for InspectTime
  void GetSetTime(shared_ptr<XdmfGrid> child, unsigned int &cnt)
  {
    if (!child->getTime())
      {
      //grid collections without explicit times are implied to go 0...N
      //so we add them here if not present
      shared_ptr<XdmfTime> time = XdmfTime::New(cnt++);
      child->setTime(time);
      }
    times.insert(child->getTime()->getValue());
  }
  //helper for InspectTime
  void GetSetTime(shared_ptr<XdmfGraph> child, unsigned int &cnt)
  {
    if (!child->getTime())
      {
      //grid collections without explicit times are implied to go 0...N
      //so we add them here if not present
      shared_ptr<XdmfTime> time = XdmfTime::New(cnt++);
      child->setTime(time);
      }
    times.insert(child->getTime()->getValue());
  }


  bool ShouldRead(unsigned int piece, unsigned int npieces)
  {
    if (this->NumProcs<1)
      {
      //no parallel information given to us, assume serial
      return true;
      }
    if (npieces == 1)
      {
      return true;
      }
    if (npieces < this->NumProcs)
      {
      if (piece == this->Rank)
        {
        return true;
        }
      return false;
      }

#if 0
    unsigned int mystart = this->Rank*npieces/this->NumProcs;
    unsigned int myend = (this->Rank+1)*npieces/this->NumProcs;
    if (piece >= mystart)
      {
      if (piece < myend || (this->Rank==this->NumProcs-1))
        {
        return true;
        }
      }
    return false;
#else
    if ((piece % this->NumProcs) == this->Rank)
      {
      return true;
      }
    else
      {
      return false;
      }
#endif

  }

  vtkXdmf3Reader_SILBuilder *SILBuilder;
  vtkXdmf3ArraySelection *FieldArrays;
  vtkXdmf3ArraySelection *CellArrays; //ie EdgeArrays for Graphs
  vtkXdmf3ArraySelection *PointArrays; //ie NodeArrays for Graphs
  vtkXdmf3ArraySelection *GridsCache;
  vtkXdmf3ArraySelection *SetsCache;
  unsigned int MaxDepth;
  unsigned int Rank;
  unsigned int NumProcs;
  std::set<double> times; //relying on implicit sort from set<double>
};

//=============================================================================
class vtkXdmfVisitor_ReadGrids
{
  //This traverses the hierarchy and reads each grid.
public:

  static shared_ptr<vtkXdmfVisitor_ReadGrids> New(
      vtkXdmf3ArraySelection *fs, vtkXdmf3ArraySelection *cs, vtkXdmf3ArraySelection *ps,
      vtkXdmf3ArraySelection *gc, vtkXdmf3ArraySelection *sc,
      unsigned int processor, unsigned int nprocessors,
      bool dt, double t,
      vtkXdmf3ArrayKeeper *keeper,
      bool asTime )
  {
    shared_ptr<vtkXdmfVisitor_ReadGrids> p(new vtkXdmfVisitor_ReadGrids());
    p->FieldArrays = fs;
    p->CellArrays = cs;
    p->PointArrays = ps;
    p->GridsCache = gc;
    p->SetsCache = sc;
    p->Rank = processor;
    p->NumProcs = nprocessors;
    p->doTime = dt;
    p->time = t;
    p->Keeper = keeper;
    p->AsTime = asTime;
    return p;
  }

  ~vtkXdmfVisitor_ReadGrids()
  {
  }

  //recursively create and populate vtk data objects for the provided Xdmf item
  vtkDataObject *Populate(shared_ptr<XdmfItem> item, vtkDataObject *toFill)
  {
    assert(toFill);

    shared_ptr<XdmfDomain> group = shared_dynamic_cast<XdmfDomain>(item);

    if (!group)
      {
      shared_ptr<XdmfUnstructuredGrid> unsGrid = shared_dynamic_cast<XdmfUnstructuredGrid>(item);
      if (unsGrid)
        {
        unsigned int nSets = unsGrid->getNumberSets();
        if (nSets > 0)
          {
          vtkMultiBlockDataSet *mbds = vtkMultiBlockDataSet::SafeDownCast(toFill);
          vtkUnstructuredGrid *child = vtkUnstructuredGrid::New();
          mbds->SetBlock
            (0,
             this->MakeUnsGrid
             (unsGrid, child, this->Keeper));
          for (unsigned int i = 0; i < nSets; i++)
            {
            vtkUnstructuredGrid *sub = vtkUnstructuredGrid::New();
            mbds->SetBlock
              (i+1,
               this->ExtractSet
               (i, unsGrid, child, sub, this->Keeper));
            sub->Delete();
            }
          child->Delete();
          return mbds;
          }
         return this->MakeUnsGrid(unsGrid, vtkUnstructuredGrid::SafeDownCast(toFill), this->Keeper);
        }

      shared_ptr<XdmfRectilinearGrid> recGrid = shared_dynamic_cast<XdmfRectilinearGrid>(item);
      if (recGrid)
        {
        unsigned int nSets = recGrid->getNumberSets();
        if (nSets > 0)
          {
          vtkMultiBlockDataSet *mbds = vtkMultiBlockDataSet::SafeDownCast(toFill);
          vtkRectilinearGrid *child = vtkRectilinearGrid::New();
          mbds->SetBlock
            (0,
             this->MakeRecGrid
             (recGrid, child, this->Keeper));
          for (unsigned int i = 0; i < nSets; i++)
            {
            vtkUnstructuredGrid *sub = vtkUnstructuredGrid::New();
            mbds->SetBlock
              (i+1,
               this->ExtractSet
               (i, recGrid, child, sub, this->Keeper));
            sub->Delete();
            }
          child->Delete();
          return mbds;
          }
        return this->MakeRecGrid(recGrid, vtkRectilinearGrid::SafeDownCast(toFill), this->Keeper);
        }

      shared_ptr<XdmfCurvilinearGrid> crvGrid = shared_dynamic_cast<XdmfCurvilinearGrid>(item);
      if (crvGrid)
        {
        unsigned int nSets = crvGrid->getNumberSets();
        if (nSets > 0)
          {
          vtkMultiBlockDataSet *mbds = vtkMultiBlockDataSet::SafeDownCast(toFill);
          vtkStructuredGrid *child = vtkStructuredGrid::New();
          mbds->SetBlock
            (0,
             this->MakeCrvGrid
             (crvGrid, child, this->Keeper));
          for (unsigned int i = 0; i < nSets; i++)
            {
            vtkUnstructuredGrid *sub = vtkUnstructuredGrid::New();
            mbds->SetBlock
              (i+1,
               this->ExtractSet
               (i, crvGrid, child, sub, this->Keeper));
            sub->Delete();
            }
          child->Delete();
          return mbds;
          }
        return this->MakeCrvGrid(crvGrid, vtkStructuredGrid::SafeDownCast(toFill), this->Keeper);
        }

      shared_ptr<XdmfRegularGrid> regGrid = shared_dynamic_cast<XdmfRegularGrid>(item);
      if (regGrid)
        {
        unsigned int nSets = regGrid->getNumberSets();
        if (nSets > 0)
          {
          vtkMultiBlockDataSet *mbds = vtkMultiBlockDataSet::SafeDownCast(toFill);
          vtkImageData *child = vtkImageData::New();
          mbds->SetBlock
            (0,
             this->MakeRegGrid
             (regGrid, child, this->Keeper));
          for (unsigned int i = 0; i < nSets; i++)
            {
            vtkUnstructuredGrid *sub = vtkUnstructuredGrid::New();
            mbds->SetBlock
              (i+1,
               this->ExtractSet
               (i, regGrid, child, sub, this->Keeper));
            sub->Delete();
            }
          child->Delete();
          return mbds;
          }
        return this->MakeRegGrid(regGrid, vtkImageData::SafeDownCast(toFill), this->Keeper);
        }

      shared_ptr<XdmfGraph> graph = shared_dynamic_cast<XdmfGraph>(item);
      if (graph)
        {
        return this->MakeGraph(graph, vtkMutableDirectedGraph::SafeDownCast(toFill), this->Keeper);
        }

      return NULL; //already spit a warning out before this
      }

    shared_ptr<XdmfGridCollection> asGC = shared_dynamic_cast<XdmfGridCollection>(item);
    bool isDomain = asGC?false:true;
    bool isTemporal = false;
    if (asGC && asGC->getType() == XdmfGridCollectionType::Temporal())
      {
      isTemporal = true;
      }

    //ignore groups that are not in timestep we were asked for
    //but be sure to return everything within them
    bool lastTime = this->doTime;
    if (this->doTime && !(isDomain || isTemporal) && asGC->getTime())
      {
      if (asGC->getTime()->getValue() != this->time)
        {
        //don't return MB that doesn't match the requested time
        return NULL;
        }

      //inside a match, make sure we get everything underneath
      this->doTime = false;
      }

    vtkMultiBlockDataSet *topB = vtkMultiBlockDataSet::SafeDownCast(toFill);
    vtkMultiPieceDataSet *topP = vtkMultiPieceDataSet::SafeDownCast(toFill);
    vtkDataObject *result;
    unsigned int cnt = 0;
    unsigned int nGridCollections = group->getNumberGridCollections();

    for (unsigned int i = 0; i < nGridCollections; i++)
      {
      if (!this->AsTime)
        {
        if (isDomain && !this->ShouldRead(i,nGridCollections))
          {
          topB->SetBlock(cnt++, NULL);
          continue;
          }
        if ((group->getGridCollection(i)->getNumberGridCollections() == 0))
          {
          vtkMultiPieceDataSet *child = vtkMultiPieceDataSet::New();
          result = this->Populate(group->getGridCollection(i), child);
          if (result)
            {
            topB->SetBlock(cnt++, result);
            }
          child->Delete();
          }
        else
          {
          vtkMultiBlockDataSet *child = vtkMultiBlockDataSet::New();
          result = this->Populate(group->getGridCollection(i), child);
          if (result)
            {
            topB->SetBlock(cnt++, result);
            }
          child->Delete();
          }
        }
      else
        {
        vtkMultiBlockDataSet *child = vtkMultiBlockDataSet::New();
        result = this->Populate(group->getGridCollection(i), child);
        if (result)
          {
          topB->SetBlock(cnt++, result);
          }
        child->Delete();
        }
      }
    unsigned int nUnstructuredGrids = group->getNumberUnstructuredGrids();
    for (unsigned int i = 0; i < nUnstructuredGrids; i++)
      {
      if (this->AsTime && !isTemporal && !this->ShouldRead(i,nUnstructuredGrids))
        {
        if (topB)
          {
          topB->SetBlock(cnt++, NULL);
          }
        else
          {
          topP->SetPiece(cnt++, NULL);
          }
        continue;
        }
      shared_ptr<XdmfUnstructuredGrid> cGrid = group->getUnstructuredGrid(i);
      unsigned int nSets = cGrid->getNumberSets();
      vtkDataObject *child;
      if (nSets > 0)
        {
        child = vtkMultiBlockDataSet::New();
        }
      else
        {
        child = vtkUnstructuredGrid::New();
        }
      result = this->Populate(group->getUnstructuredGrid(i), child);
      if (result)
        {
        if (topB)
          {
          topB->SetBlock(cnt++, result);
          }
        else
          {
          topP->SetPiece(cnt++, result);
          }
        }
      child->Delete();
      }
    unsigned int nRectilinearGrids = group->getNumberRectilinearGrids();
    for (unsigned int i = 0; i < nRectilinearGrids; i++)
      {
      if (this->AsTime && !isTemporal && !this->ShouldRead(i,nRectilinearGrids))
        {
        if (topB)
          {
          topB->SetBlock(cnt++, NULL);
          }
        else
          {
          topP->SetPiece(cnt++, NULL);
          }
        continue;
        }
      shared_ptr<XdmfRectilinearGrid> cGrid = group->getRectilinearGrid(i);
      unsigned int nSets = cGrid->getNumberSets();
      vtkDataObject *child;
      if (nSets > 0)
        {
        child = vtkMultiBlockDataSet::New();
        }
      else
        {
        child = vtkRectilinearGrid::New();
        }
      result = this->Populate(cGrid, child);
      if (result)
        {
        if (topB)
          {
          topB->SetBlock(cnt++, result);
          }
        else
          {
          topP->SetPiece(cnt++, result);
          }
        }
      child->Delete();
      }
    unsigned int nCurvilinearGrids= group->getNumberCurvilinearGrids();
    for (unsigned int i = 0; i < nCurvilinearGrids; i++)
      {
      if (this->AsTime && !isTemporal && !this->ShouldRead(i,nCurvilinearGrids))
        {
        if (topB)
          {
          topB->SetBlock(cnt++, NULL);
          }
        else
          {
          topP->SetPiece(cnt++, NULL);
          }
        continue;
        }
      shared_ptr<XdmfCurvilinearGrid> cGrid = group->getCurvilinearGrid(i);
      unsigned int nSets = cGrid->getNumberSets();
      vtkDataObject *child;
      if (nSets > 0)
        {
        child = vtkMultiBlockDataSet::New();
        }
      else
        {
        child = vtkStructuredGrid::New();
        }
      result = this->Populate(cGrid, child);
      if (result)
        {
        if (topB)
          {
          topB->SetBlock(cnt++, result);
          }
        else
          {
          topP->SetPiece(cnt++, result);
          }
        }
      child->Delete();
      }
    unsigned int nRegularGrids = group->getNumberRegularGrids();
    for (unsigned int i = 0; i < nRegularGrids; i++)
      {
      if (this->AsTime && !isTemporal && !this->ShouldRead(i,nRegularGrids))
        {
        if (topB)
          {
          topB->SetBlock(cnt++, NULL);
          }
        else
          {
          topP->SetPiece(cnt++, NULL);
          }
        continue;
        }
      shared_ptr<XdmfRegularGrid> cGrid = group->getRegularGrid(i);
      unsigned int nSets = cGrid->getNumberSets();
      vtkDataObject *child;
      if (nSets > 0)
        {
        child = vtkMultiBlockDataSet::New();
        }
      else
        {
        child = vtkUniformGrid::New();
        }
      result = this->Populate(cGrid, child);
      if (result)
        {
        if (topB)
          {
          topB->SetBlock(cnt++, result);
          }
        else
          {
          topP->SetPiece(cnt++, result);
          }
        }
      child->Delete();
      }
    unsigned int nGraphs = group->getNumberGraphs();
    for (unsigned int i = 0; i < nGraphs; i++)
      {
      if (this->AsTime && !isTemporal && !this->ShouldRead(i,nGraphs))
        {
        if (topB)
          {
          topB->SetBlock(cnt++, NULL);
          }
        else
          {
          topP->SetPiece(cnt++, NULL);
          }
        continue;
        }
      vtkMutableDirectedGraph *child = vtkMutableDirectedGraph::New();
      result = this->Populate(group->getGraph(i), child);
      if (result)
        {
        if (topB)
          {
          topB->SetBlock(cnt++, result);
          }
        else
          {
          topP->SetPiece(cnt++, result);
          }
        }
      child->Delete();
      }

    if (lastTime)
      {
      //restore time search now that we've done the group contents
      this->doTime = true;
      }

    if (isTemporal && topB->GetNumberOfBlocks()==1)
      {
      //temporal collection is just a place holder for its content
      return topB->GetBlock(0);
      }

    if (topB)
      return topB;
    else
      return topP;
  }

  vtkXdmf3ArrayKeeper* Keeper;

protected:
  vtkXdmfVisitor_ReadGrids()
  {
  }
  bool ShouldRead(unsigned int piece, unsigned int npieces)
  {
    if (this->NumProcs<1)
      {
      //no parallel information given to us, assume serial
      return true;
      }
    if (npieces == 1)
      {
      return true;
      }
    if (npieces < this->NumProcs)
      {
      if (piece == this->Rank)
        {
        return true;
        }
      return false;
      }

#if 1
    unsigned int mystart = this->Rank*npieces/this->NumProcs;
    unsigned int myend = (this->Rank+1)*npieces/this->NumProcs;
    if (piece >= mystart)
      {
      if (piece < myend || (this->Rank==this->NumProcs-1))
        {
        return true;
        }
      }
    return false;
#else
    if ((piece % this->NumProcs) == this->Rank)
      {
      return true;
      }
    else
      {
      return false;
      }
#endif

  }

  bool GridEnabled(shared_ptr<XdmfGrid> grid)
  {
    return this->GridsCache->ArrayIsEnabled(grid->getName().c_str());
  }
  bool GridEnabled(shared_ptr<XdmfGraph> graph)
  {
    return this->GridsCache->ArrayIsEnabled(graph->getName().c_str());
  }

  bool SetEnabled(shared_ptr<XdmfSet> set)
  {
    return this->SetsCache->ArrayIsEnabled(set->getName().c_str());
  }

  bool ForThisTime(shared_ptr<XdmfGrid> grid)
  {
    return (!this->doTime ||
            (grid->getTime() &&
             grid->getTime()->getValue() == this->time));
  }
  bool ForThisTime(shared_ptr<XdmfGraph> graph)
  {
    return (!this->doTime ||
            (graph->getTime() &&
             graph->getTime()->getValue() == this->time));
  }

  vtkDataObject *MakeUnsGrid(shared_ptr<XdmfUnstructuredGrid> grid, vtkUnstructuredGrid *dataSet, vtkXdmf3ArrayKeeper *keeper)
  {
    if (dataSet && GridEnabled(grid) && ForThisTime(grid))
      {
      vtkXdmf3DataSet::XdmfToVTK(
        this->FieldArrays, this->CellArrays, this->PointArrays,
        grid.get(), dataSet, keeper);
      return dataSet;
      }
    return NULL;
  }

  vtkDataObject *MakeRecGrid(shared_ptr<XdmfRectilinearGrid> grid, vtkRectilinearGrid *dataSet, vtkXdmf3ArrayKeeper *keeper)
  {
    if (dataSet && GridEnabled(grid) && ForThisTime(grid))
      {
      vtkXdmf3DataSet::XdmfToVTK(
        this->FieldArrays, this->CellArrays, this->PointArrays,
        grid.get(), dataSet, keeper);
      return dataSet;
      }
    return NULL;
  }

  vtkDataObject *MakeCrvGrid(shared_ptr<XdmfCurvilinearGrid> grid, vtkStructuredGrid *dataSet, vtkXdmf3ArrayKeeper *keeper)
    {
      if (dataSet && GridEnabled(grid) && ForThisTime(grid))
        {
        vtkXdmf3DataSet::XdmfToVTK(
          this->FieldArrays, this->CellArrays, this->PointArrays,
          grid.get(), dataSet, keeper);
        return dataSet;
        }
      return NULL;
    }

  vtkDataObject *MakeRegGrid(shared_ptr<XdmfRegularGrid> grid, vtkImageData *dataSet, vtkXdmf3ArrayKeeper *keeper)
  {
    if (dataSet && GridEnabled(grid) && ForThisTime(grid))
      {
      vtkXdmf3DataSet::XdmfToVTK(
        this->FieldArrays, this->CellArrays, this->PointArrays,
        grid.get(), dataSet, keeper);
      return dataSet;
      }
    return NULL;
  }

  vtkDataObject *MakeGraph(shared_ptr<XdmfGraph> grid, vtkMutableDirectedGraph *dataSet, vtkXdmf3ArrayKeeper *keeper)
  {
    if (dataSet && GridEnabled(grid) && ForThisTime(grid))
      {
      vtkXdmf3DataSet::XdmfToVTK(
        this->FieldArrays, this->CellArrays, this->PointArrays,
        grid.get(), dataSet, keeper);
      return dataSet;
      }
    return NULL;
  }

  vtkDataObject *ExtractSet(unsigned int setnum, shared_ptr<XdmfGrid> grid,
                            vtkDataSet *dataSet,
                            vtkUnstructuredGrid *subSet, vtkXdmf3ArrayKeeper *keeper)
  {
    shared_ptr<XdmfSet> set = grid->getSet(setnum);
    if (dataSet && subSet && SetEnabled(set) && ForThisTime(grid))
      {
      vtkXdmf3DataSet::XdmfSubsetToVTK(
        grid.get(), setnum, dataSet, subSet, keeper);
      return subSet;
      }
    return NULL;
  }


  bool doTime;
  double time;
  unsigned int Rank;
  unsigned int NumProcs;
  vtkXdmf3ArraySelection* FieldArrays;
  vtkXdmf3ArraySelection* CellArrays;
  vtkXdmf3ArraySelection* PointArrays;
  vtkXdmf3ArraySelection* GridsCache;
  vtkXdmf3ArraySelection* SetsCache;
  bool AsTime;
};

//=============================================================================
class vtkXdmf3Reader::Internals
{
  //Private implementation details for vtkXdmf3Reader
public:
  Internals()
  {
    this->FieldArrays = new vtkXdmf3ArraySelection;
    this->CellArrays = new vtkXdmf3ArraySelection;
    this->PointArrays = new vtkXdmf3ArraySelection;
    this->GridsCache = new vtkXdmf3ArraySelection;
    this->SetsCache = new vtkXdmf3ArraySelection;

    this->SILBuilder = new vtkXdmf3Reader_SILBuilder();
    this->SILBuilder->Initialize();

    this->Keeper = new vtkXdmf3ArrayKeeper;
  };

  ~Internals()
  {
    delete this->FieldArrays;
    delete this->CellArrays;
    delete this->PointArrays;
    delete this->GridsCache;
    delete this->SetsCache;
    delete this->SILBuilder;
    this->ReleaseArrays(true);
    this->FileNames.clear();
  };

  //--------------------------------------------------------------------------
  bool PrepareDocument(vtkXdmf3Reader *self, const char *FileName, bool AsTime)
  {
    if (this->Domain)
      {
      return true;
      }

    if (!FileName )
      {
      vtkErrorWithObjectMacro(self, "File name not set");
      return false;
      }
    if (!vtksys::SystemTools::FileExists(FileName))
      {
      vtkErrorWithObjectMacro(self, "Error opening file " << FileName);
      return false;
      }
    if (!this->Domain)
      {
      this->Init(FileName, AsTime);
      }
    return true;
  }

  //--------------------------------------------------------------------------
  vtkGraph *GetSIL()
  {
    return this->SILBuilder->SIL;
  }

  //--------------------------------------------------------------------------
  //find out what kind of vtkdataobject the xdmf file will produce
  int GetVTKType()
  {
    if (this->VTKType != -1)
      {
      return this->VTKType;
      }
    unsigned int nGridCollections = this->Domain->getNumberGridCollections();
    if (nGridCollections > 1)
      {
      this->VTKType = VTK_MULTIBLOCK_DATA_SET;
      return this->VTKType;
      }

    //check for temporal of atomic, in which case we produce the atomic type
    shared_ptr<XdmfDomain> toCheck = this->Domain;
    bool temporal = false;
    if (nGridCollections == 1)
      {
      shared_ptr<XdmfGridCollection> gc = this->Domain->getGridCollection(0);
      if (gc->getType() == XdmfGridCollectionType::Temporal())
        {
        if (gc->getNumberGridCollections() == 0)
          {
          temporal = true;
          toCheck = gc;
          }
        }
      }

    unsigned int nUnstructuredGrids = toCheck->getNumberUnstructuredGrids();
    unsigned int nRectilinearGrids = toCheck->getNumberRectilinearGrids();
    unsigned int nCurvilinearGrids= toCheck->getNumberCurvilinearGrids();
    unsigned int nRegularGrids = toCheck->getNumberRegularGrids();
    unsigned int nGraphs = toCheck->getNumberGraphs();
    int numtypes = 0;
    numtypes = numtypes + (nUnstructuredGrids>0?1:0);
    numtypes = numtypes + (nRectilinearGrids>0?1:0);
    numtypes = numtypes + (nCurvilinearGrids>0?1:0);
    numtypes = numtypes + (nRegularGrids>0?1:0);
    numtypes = numtypes + (nGraphs>0?1:0);
    bool atomic =
        temporal ||
        (numtypes==1 &&
          (
          nUnstructuredGrids==1||
          nRectilinearGrids==1||
          nCurvilinearGrids==1||
          nRegularGrids==1||
          nGraphs==1));
    if (!atomic
        )
      {
      this->VTKType = VTK_MULTIBLOCK_DATA_SET;
      }
    else
      {
      this->VTKType = VTK_UNIFORM_GRID;
      this->TopGrid = toCheck->getRegularGrid(0); //keep a reference to get extent from
      if (nRectilinearGrids>0)
        {
        this->VTKType = VTK_RECTILINEAR_GRID;
        this->TopGrid = toCheck->getRectilinearGrid(0);//keep a reference to get extent from
        }
      else if (nCurvilinearGrids>0)
        {
        this->VTKType = VTK_STRUCTURED_GRID;
        this->TopGrid = toCheck->getCurvilinearGrid(0);//keep a reference to get extent from
        }
      else if (nUnstructuredGrids>0)
        {
        this->VTKType = VTK_UNSTRUCTURED_GRID;
        this->TopGrid = toCheck->getUnstructuredGrid(0);
        }
      else if (nGraphs>0)
        {
        this->VTKType = VTK_DIRECTED_GRAPH; //VTK_MUTABLE_DIRECTED_GRAPH more specifically
        }
      }
      if (this->TopGrid)
        {
        shared_ptr<XdmfGrid> grid =
          shared_dynamic_cast<XdmfGrid>(this->TopGrid);
        if (grid && grid->getNumberSets()>0)
          {
          this->VTKType = VTK_MULTIBLOCK_DATA_SET;
          }
        }
     return this->VTKType;
  }

  //--------------------------------------------------------------------------
  void ReadHeavyData(unsigned int updatePiece, unsigned int updateNumPieces,
                     bool doTime, double time, vtkMultiBlockDataSet* mbds,
                     bool AsTime)
  {
    //traverse the xdmf hierarchy, and convert and return what was requested
    shared_ptr<vtkXdmfVisitor_ReadGrids> visitor =
        vtkXdmfVisitor_ReadGrids::New(
          this->FieldArrays,
          this->CellArrays,
          this->PointArrays,
          this->GridsCache,
          this->SetsCache,
          updatePiece,
          updateNumPieces,
          doTime,
          time,
          this->Keeper,
          AsTime
          );
      visitor->Populate(this->Domain, mbds);
  }

  //--------------------------------------------------------------------------
  void ReleaseArrays(bool force=false)
  {
    if (!this->Keeper)
      {
      return;
      }
    this->Keeper->Release(force);
  }

  //--------------------------------------------------------------------------
  void BumpKeeper()
  {
    if (!this->Keeper)
      {
      return;
      }
    this->Keeper->BumpGeneration();
  }

  vtkXdmf3ArraySelection *FieldArrays;
  vtkXdmf3ArraySelection *CellArrays;
  vtkXdmf3ArraySelection *PointArrays;
  vtkXdmf3ArraySelection *GridsCache;
  vtkXdmf3ArraySelection *SetsCache;
  std::vector<double> TimeSteps;
  shared_ptr<XdmfItem> TopGrid;
  vtkXdmf3ArrayKeeper *Keeper;

  std::vector<std::string> FileNames;

private:

  //--------------------------------------------------------------------------
  void Init(const char *filename, bool AsTime)
  {
    vtkTimerLog::MarkStartEvent("X3R::Init");
    unsigned int idx = this->FileNames.size();
    assert(idx > 0);

    this->Reader = XdmfReader::New();

    unsigned int updatePiece = 0;
    unsigned int updateNumPieces = 1;
    vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
    if (ctrl != NULL)
      {
      updatePiece = ctrl->GetLocalProcessId();
      updateNumPieces = ctrl->GetNumberOfProcesses();
      }
    else
      {
      updatePiece = 0;
      updateNumPieces = 1;
      }

    if (idx == 1)
      {
      this->Domain = shared_dynamic_cast<XdmfDomain>
        (this->Reader->read(filename));
      }
    else
      {
      this->Domain = XdmfDomain::New();
      shared_ptr<XdmfGridCollection> topc = XdmfGridCollection::New();
      if (AsTime)
        {
        topc->setType(XdmfGridCollectionType::Temporal());
        }
      this->Domain->insert(topc);
      for (int i = 0; i < idx; i++)
        {
        if (AsTime || (i%updateNumPieces == updatePiece))
          {
          shared_ptr<XdmfDomain> fdomain = shared_dynamic_cast<XdmfDomain>
            (this->Reader->read(this->FileNames[i]));

          for (unsigned int i = 0; i < fdomain->getNumberGridCollections(); i++)
            {
            topc->insert(fdomain->getGridCollection(i));
            }
          for (unsigned int i = 0; i < fdomain->getNumberUnstructuredGrids(); i++)
            {
            topc->insert(fdomain->getUnstructuredGrid(i));
            }
          for (unsigned int i = 0; i < fdomain->getNumberRectilinearGrids(); i++)
            {
            topc->insert(fdomain->getRectilinearGrid(i));
            }
          for (unsigned int i = 0; i < fdomain->getNumberCurvilinearGrids(); i++)
            {
            topc->insert(fdomain->getCurvilinearGrid(i));
            }
          for (unsigned int i = 0; i < fdomain->getNumberRegularGrids(); i++)
            {
            topc->insert(fdomain->getRegularGrid(i));
            }
          for (unsigned int i = 0; i < fdomain->getNumberGraphs(); i++)
            {
            topc->insert(fdomain->getGraph(i));
            }
          }
        }
      }

    this->VTKType = -1;
    vtkTimerLog::MarkStartEvent("X3R::learn");
    this->GatherMetaInformation();
    vtkTimerLog::MarkEndEvent("X3R::learn");

    vtkTimerLog::MarkEndEvent("X3R::Init");
  }

  //--------------------------------------------------------------------------
  void GatherMetaInformation()
  {
    vtkTimerLog::MarkStartEvent("X3R::GatherMetaInfo");

    unsigned int updatePiece = 0;
    unsigned int updateNumPieces = 1;
    vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
    if (ctrl != NULL)
      {
      updatePiece = ctrl->GetLocalProcessId();
      updateNumPieces = ctrl->GetNumberOfProcesses();
      cerr << "RDO " << updatePiece << "/" << updateNumPieces << endl;
      }
    else
      {
      cerr << "ROH ROH RAGGIE" << endl;
      updatePiece = 0;
      updateNumPieces = 1;
      }
    shared_ptr<vtkXdmfVisitor_Translator> visitor =
          vtkXdmfVisitor_Translator::New (
              this->SILBuilder,
              this->FieldArrays,
              this->CellArrays,
              this->PointArrays,
              this->GridsCache,
              this->SetsCache,
              updatePiece,
              updateNumPieces);
    visitor->InspectXDMF(this->Domain, -1);
    visitor->ClearGridsIfNeeded(this->Domain);
    if (this->TimeSteps.size())
       {
       this->TimeSteps.erase(this->TimeSteps.begin());
       }
     std::set<double> times = visitor->getTimes();
     std::set<double>::const_iterator it = times.begin();
     while (it != times.end())
       {
       this->TimeSteps.push_back(*it);
       it++;
       }
    vtkTimerLog::MarkEndEvent("X3R::GatherMetaInfo");
  }

  int VTKType;
  shared_ptr<XdmfReader> Reader;
  shared_ptr<XdmfDomain> Domain;
  vtkXdmf3Reader_SILBuilder *SILBuilder;
};

//==============================================================================

vtkStandardNewMacro(vtkXdmf3Reader);

//----------------------------------------------------------------------------
vtkXdmf3Reader::vtkXdmf3Reader()
{
  this->FileName = NULL;

  this->Internal = new vtkXdmf3Reader::Internals();
  this->FileSeriesAsTime = true;

  this->FieldArraysCache = this->Internal->FieldArrays;
  this->CellArraysCache = this->Internal->CellArrays;
  this->PointArraysCache = this->Internal->PointArrays;
  this->SetsCache = this->Internal->SetsCache;
  this->GridsCache = this->Internal->GridsCache;
}

//----------------------------------------------------------------------------
vtkXdmf3Reader::~vtkXdmf3Reader()
{

  this->SetFileName(NULL);
  delete this->Internal;
  //XdmfHDF5Controller::closeFiles();
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
  os << indent << "FileName: " <<
    (this->FileName ? this->FileName : "(none)") << endl;
  os << indent << "FileSeriesAsTime: " <<
    (this->FileSeriesAsTime ? "True" : "False") << endl;
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::AddFileName(const char* filename)
{
  this->Internal->FileNames.push_back(filename);
  if (this->Internal->FileNames.size()==1)
    {
    this->Superclass::SetFileName(filename);
    }
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::SetFileName(const char* filename)
{
  this->RemoveAllFileNames();
  this->Superclass::SetFileName(filename);
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::RemoveAllFileNames()
{
  this->Internal->FileNames.clear();
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::CanReadFile(const char* filename)
{
  if (!vtksys::SystemTools::FileExists(filename))
    {
    return 0;
    }

 /*
  TODO: this, but really fast
  shared_ptr<XdmfReader> tester = XdmfReader::New();
  try {
    shared_ptr<XdmfItem> item = tester->read(filename);
  } catch (XdmfError & e) {
    return 0;
  }
 */

  return 1;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::FillOutputPortInformation(int, vtkInformation *info)
{
  info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkDataObject");
  return 1;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::ProcessRequest(vtkInformation *request,
    vtkInformationVector **inputVector,
    vtkInformationVector *outputVector)
{
  // create the output
  if (request->Has(vtkDemandDrivenPipeline::REQUEST_DATA_OBJECT()))
    {
    return this->RequestDataObject(outputVector);
    }

  return this->Superclass::ProcessRequest(request, inputVector, outputVector);
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::RequestDataObject(vtkInformationVector *outputVector)
{
  vtkTimerLog::MarkStartEvent("X3R::RDO");
  //let libXdmf parse XML
  if (!this->Internal->PrepareDocument(this, this->FileName, this->FileSeriesAsTime))
    {
    vtkTimerLog::MarkEndEvent("X3R::RDO");
    return 0;
    }

  //Determine what vtkDataObject we should produce
  int vtk_type = this->Internal->GetVTKType();

  //Make an empty vtkDataObject
  vtkDataObject* output = vtkDataObject::GetData(outputVector, 0);
  if (!output || output->GetDataObjectType() != vtk_type)
    {
    if (vtk_type == VTK_DIRECTED_GRAPH)
      {
      output = vtkMutableDirectedGraph::New();
      }
    else
      {
      output = vtkDataObjectTypes::NewDataObject(vtk_type);
      }
    outputVector->GetInformationObject(0)->Set(
        vtkDataObject::DATA_OBJECT(), output );
    this->GetOutputPortInformation(0)->Set(
      vtkDataObject::DATA_EXTENT_TYPE(), output->GetExtentType());
    output->Delete();
    }

  vtkTimerLog::MarkEndEvent("X3R::RDO");
  return 1;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::RequestInformation(vtkInformation *,
  vtkInformationVector **,
  vtkInformationVector *outputVector)
{
  vtkTimerLog::MarkStartEvent("X3R::RI");
  if (!this->Internal->PrepareDocument(this, this->FileName, this->FileSeriesAsTime))
    {
    vtkTimerLog::MarkEndEvent("X3R::RI");
    return 0;
    }

  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  // Publish the fact that this reader can satisfy any piece request.
  outInfo->Set(CAN_HANDLE_PIECE_REQUEST(), 1);

  // Publish the SIL which provides information about the grid hierarchy.
  outInfo->Set(vtkDataObject::SIL(), this->Internal->GetSIL());

  // Publish the times that we have data for
  if (this->Internal->TimeSteps.size() > 0)
    {
    outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
      &this->Internal->TimeSteps[0],
      static_cast<int>(this->Internal->TimeSteps.size()));
    double timeRange[2];
    timeRange[0] = this->Internal->TimeSteps.front();
    timeRange[1] = this->Internal->TimeSteps.back();
    outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(), timeRange, 2);
    }

  // Structured atomic must announce the whole extent it can provide
  int vtk_type = this->Internal->GetVTKType();
  if (vtk_type == VTK_STRUCTURED_GRID ||
      vtk_type == VTK_RECTILINEAR_GRID ||
      vtk_type == VTK_IMAGE_DATA ||
      vtk_type == VTK_UNIFORM_GRID)
    {
    int whole_extent[6];
    double origin[3];
    double spacing[3];
    whole_extent[0] = 0;
    whole_extent[1] = -1;
    whole_extent[2] = 0;
    whole_extent[3] = -1;
    whole_extent[4] = 0;
    whole_extent[5] = -1;
    origin[0] = 0.0;
    origin[1] = 0.0;
    origin[2] = 0.0;
    spacing[0] = 1.0;
    spacing[1] = 1.0;
    spacing[2] = 1.0;

    shared_ptr<XdmfRegularGrid> regGrid =
      shared_dynamic_cast<XdmfRegularGrid>(this->Internal->TopGrid);
    if (regGrid)
      {
      vtkImageData *dataSet = vtkImageData::New();
      vtkXdmf3DataSet::CopyShape(regGrid.get(), dataSet, this->Internal->Keeper);
      dataSet->GetExtent(whole_extent);
      dataSet->GetOrigin(origin);
      dataSet->GetSpacing(spacing);
      dataSet->Delete();
      }
    shared_ptr<XdmfRectilinearGrid> recGrid =
      shared_dynamic_cast<XdmfRectilinearGrid>(this->Internal->TopGrid);
    if (recGrid)
      {
      vtkRectilinearGrid *dataSet = vtkRectilinearGrid::New();
      vtkXdmf3DataSet::CopyShape(recGrid.get(), dataSet, this->Internal->Keeper);
      dataSet->GetExtent(whole_extent);
      dataSet->Delete();
      }
    shared_ptr<XdmfCurvilinearGrid> crvGrid =
      shared_dynamic_cast<XdmfCurvilinearGrid>(this->Internal->TopGrid);
    if (crvGrid)
      {
      vtkStructuredGrid *dataSet = vtkStructuredGrid::New();
      vtkXdmf3DataSet::CopyShape(crvGrid.get(), dataSet, this->Internal->Keeper);
      dataSet->GetExtent(whole_extent);
      dataSet->Delete();
      }

    outInfo->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
        whole_extent, 6);
    outInfo->Set(vtkDataObject::ORIGIN(), origin, 3);
    outInfo->Set(vtkDataObject::SPACING(), spacing, 3);
    }

  vtkTimerLog::MarkEndEvent("X3R::RI");
  return 1;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::RequestData(vtkInformation *,
  vtkInformationVector **,
  vtkInformationVector *outputVector)
{
  vtkTimerLog::MarkStartEvent("X3R::RD");

  if (!this->Internal->PrepareDocument(this, this->FileName, this->FileSeriesAsTime))
    {
    vtkTimerLog::MarkEndEvent("X3R::RD");
    return 0;
    }

  vtkTimerLog::MarkStartEvent("X3R::Release");
  this->Internal->ReleaseArrays();
  this->Internal->BumpKeeper();
  vtkTimerLog::MarkEndEvent("X3R::Release");

  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  // Collect information about what spatial extent is requested.
  unsigned int updatePiece = 0;
  unsigned int updateNumPieces = 1;
  if (outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER()) &&
      outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_PIECES()))
    {
    updatePiece = static_cast<unsigned int>(
        outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER()));
    updateNumPieces =  static_cast<unsigned int>(
        outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_PIECES()));
    }
  /*
  int ghost_levels = 0;
  if (outInfo->Has(
      vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_GHOST_LEVELS()))
    {
    ghost_levels = outInfo->Get(
        vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_GHOST_LEVELS());
    }
  */
  /*
  int update_extent[6] = {0, -1, 0, -1, 0, -1};
  if (outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT()))
    {
    outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(),
        update_extent);
    }
  */

  // Collect information about what temporal extent is requested.
  double time = 0.0;
  bool doTime = false;
  if (outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP()) &&
      this->Internal->TimeSteps.size())
    {
    doTime = true;
    time =
      outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP());
    //find the nearest match (floor), so we have something exact to search for
    std::vector<double>::iterator it = upper_bound(
      this->Internal->TimeSteps.begin(), this->Internal->TimeSteps.end(), time);
    if (it != this->Internal->TimeSteps.begin())
      {
      it--;
      }
    time = *it;
    }

  vtkDataObject* output = vtkDataObject::GetData(outInfo);
  if (!output)
    {
    return 0;
    }
  if (doTime)
    {
    output->GetInformation()->Set(vtkDataObject::DATA_TIME_STEP(), time);
    }

  vtkMultiBlockDataSet *mbds = vtkMultiBlockDataSet::New();
  this->Internal->ReadHeavyData(
      updatePiece, updateNumPieces,
      doTime, time,
      mbds,
      this->FileSeriesAsTime);
  if (mbds->GetNumberOfBlocks()==1)
    {
    output->ShallowCopy(mbds->GetBlock(0));
    }
  else
    {
    output->ShallowCopy(mbds);
    }
  mbds->Delete();

  vtkTimerLog::MarkEndEvent("X3R::RD");

  return 1;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetNumberOfFieldArrays()
{
  return this->GetFieldArraySelection()->GetNumberOfArrays();
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::SetFieldArrayStatus(const char* arrayname, int status)
{
  this->GetFieldArraySelection()->SetArrayStatus(arrayname, status != 0);
  this->Modified();
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetFieldArrayStatus(const char* arrayname)
{
  return this->GetFieldArraySelection()->GetArraySetting(arrayname);
}

//----------------------------------------------------------------------------
const char* vtkXdmf3Reader::GetFieldArrayName(int index)
{
  return this->GetFieldArraySelection()->GetArrayName(index);
}

//----------------------------------------------------------------------------
vtkXdmf3ArraySelection* vtkXdmf3Reader::GetFieldArraySelection()
{
  return this->FieldArraysCache;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetNumberOfCellArrays()
{
  return this->GetCellArraySelection()->GetNumberOfArrays();
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::SetCellArrayStatus(const char* arrayname, int status)
{
  this->GetCellArraySelection()->SetArrayStatus(arrayname, status != 0);
  this->Modified();
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetCellArrayStatus(const char* arrayname)
{
  return this->GetCellArraySelection()->GetArraySetting(arrayname);
}

//----------------------------------------------------------------------------
const char* vtkXdmf3Reader::GetCellArrayName(int index)
{
  return this->GetCellArraySelection()->GetArrayName(index);
}

//----------------------------------------------------------------------------
vtkXdmf3ArraySelection* vtkXdmf3Reader::GetCellArraySelection()
{
  return this->CellArraysCache;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetNumberOfPointArrays()
{
  return this->GetPointArraySelection()->GetNumberOfArrays();
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::SetPointArrayStatus(const char* arrayname, int status)
{
  this->GetPointArraySelection()->SetArrayStatus(arrayname, status != 0);
  this->Modified();
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetPointArrayStatus(const char* arrayname)
{
  return this->GetPointArraySelection()->GetArraySetting(arrayname);
}

//----------------------------------------------------------------------------
const char* vtkXdmf3Reader::GetPointArrayName(int index)
{
  return this->GetPointArraySelection()->GetArrayName(index);
}

//----------------------------------------------------------------------------
vtkXdmf3ArraySelection* vtkXdmf3Reader::GetPointArraySelection()
{
  return this->PointArraysCache;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetNumberOfGrids()
{
  return this->GetGridsSelection()->GetNumberOfArrays();
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::SetGridStatus(const char* gridname, int status)
{
  this->GetGridsSelection()->SetArrayStatus(gridname, status !=0);
  this->Modified();
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetGridStatus(const char* arrayname)
{
  return this->GetGridsSelection()->GetArraySetting(arrayname);
}

//----------------------------------------------------------------------------
const char* vtkXdmf3Reader::GetGridName(int index)
{
  return this->GetGridsSelection()->GetArrayName(index);
}

//----------------------------------------------------------------------------
vtkXdmf3ArraySelection* vtkXdmf3Reader::GetGridsSelection()
{
  return this->GridsCache;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetNumberOfSets()
{
  return this->GetSetsSelection()->GetNumberOfArrays();
}

//----------------------------------------------------------------------------
void vtkXdmf3Reader::SetSetStatus(const char* arrayname, int status)
{
  this->GetSetsSelection()->SetArrayStatus(arrayname, status != 0);
  this->Modified();
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetSetStatus(const char* arrayname)
{
  return this->GetSetsSelection()->GetArraySetting(arrayname);
}

//----------------------------------------------------------------------------
const char* vtkXdmf3Reader::GetSetName(int index)
{
  return this->GetSetsSelection()->GetArrayName(index);
}

//----------------------------------------------------------------------------
vtkXdmf3ArraySelection* vtkXdmf3Reader::GetSetsSelection()
{
  return this->SetsCache;
}

//----------------------------------------------------------------------------
vtkGraph* vtkXdmf3Reader::GetSIL()
{
  vtkGraph * ret = this->Internal->GetSIL();
  return ret;
}

//----------------------------------------------------------------------------
int vtkXdmf3Reader::GetSILUpdateStamp()
{
  return this->Internal->GetSIL()->GetMTime();
}
