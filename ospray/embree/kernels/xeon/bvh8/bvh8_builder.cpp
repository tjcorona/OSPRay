// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh8.h"
#include "bvh8_builder.h"
//#include "bvh8_refit.h"
//#include "bvh8_rotate.h"
#include "bvh8_statistics.h"

#include "geometry/triangle1.h"
#include "geometry/triangle4.h"
#include "geometry/triangle8.h"
#include "geometry/triangle1v.h"
#include "geometry/triangle4v.h"
#include "geometry/triangle4i.h"

#define ROTATE_TREE 0

#include "common/scene_triangle_mesh.h"

namespace embree
{
  namespace isa
  {
    static const size_t THRESHOLD_FOR_SINGLE_THREADED = 50000; // FIXME: measure if this is really optimal, maybe disable only parallel splits

    template<> BVH8BuilderT<Triangle4 >::BVH8BuilderT (BVH8* bvh, Scene* scene, size_t mode) 
      : BVH8Builder(bvh,scene,NULL,mode,2,2,1.0f,false,sizeof(Triangle4),4,inf) {}

    template<> BVH8BuilderT<Triangle8 >::BVH8BuilderT (BVH8* bvh, Scene* scene, size_t mode) 
      : BVH8Builder(bvh,scene,NULL,mode,3,2,1.0f,false,sizeof(Triangle8),8,inf) {}

    BVH8Builder::BVH8Builder (BVH8* bvh, Scene* scene, TriangleMesh* mesh, size_t mode,
				size_t logBlockSize, size_t logSAHBlockSize, float intCost, 
				bool needVertices, size_t primBytes, const size_t minLeafSize, const size_t maxLeafSize)
      : scene(scene), mesh(mesh), bvh(bvh), scheduler(&scene->lockstep_scheduler), enableSpatialSplits(mode & MODE_HIGH_QUALITY), listMode(mode & LIST_MODE_BITS), remainingReplications(0),
	logBlockSize(logBlockSize), logSAHBlockSize(logSAHBlockSize), intCost(intCost), 
	needVertices(needVertices), primBytes(primBytes), minLeafSize(minLeafSize), maxLeafSize(maxLeafSize)
     {
       size_t maxLeafPrims = BVH8::maxLeafBlocks*(size_t(1)<<logBlockSize);
       if (maxLeafPrims < this->maxLeafSize) this->maxLeafSize = maxLeafPrims;
       needAllThreads = true;
    }
    
    BVH8Builder::~BVH8Builder() {
      bvh->alloc.shrink();
    }

    template<typename Triangle>
    typename BVH8Builder::NodeRef BVH8BuilderT<Triangle>::createLeaf(size_t threadIndex, Allocator& nodeAlloc, Allocator& leafAlloc, PrimRefList& prims, const PrimInfo& pinfo)
    {
      /* allocate leaf node */
      size_t N = blocks(pinfo.size());
      Triangle* leaf = (Triangle*) bvh->allocPrimitiveBlocks(nodeAlloc,N); // FIXME: should be leafAlloc?
      assert(N <= (size_t)BVH8::maxLeafBlocks);

      /* make order of primitives deterministic */
      PrimRefList::item* block = prims.take();
      if (block) {
	while (PrimRefList::item* block1 = prims.take()) {
	  for (size_t i=0; i<block1->size(); i++) {
	    bool ok = block->insert(block1->at(i));
	    assert(ok);
	  }
	  alloc.free(threadIndex,block1);
	}
	std::sort(&block->at(0),&block->at(block->size()));
	prims.insert(block);
      }

      /* insert all triangles */
      PrimRefList::block_iterator_unsafe iter(prims);
      for (size_t i=0; i<N; i++) leaf[i].fill(iter,scene,listMode);
      assert(!iter);
      
      /* free all primitive blocks */
      while (PrimRefList::item* block = prims.take())
	alloc.free(threadIndex,block);

      return bvh->encodeLeaf(leaf,listMode ? listMode : N);
    }
    
    BVH8Builder::NodeRef BVH8Builder::createLargeLeaf(size_t threadIndex, Allocator& nodeAlloc, Allocator& leafAlloc,
                                                      PrimRefList& prims, const PrimInfo& pinfo, size_t depth)
    {
#if defined(_DEBUG)
      if (depth >= BVH8::maxBuildDepthLeaf) 
	THROW_RUNTIME_ERROR("ERROR: Loosing primitives during build.");
#endif
      
      /* create leaf for few primitives */
      if (pinfo.size() <= maxLeafSize)
	return createLeaf(threadIndex,nodeAlloc,leafAlloc,prims,pinfo);
      
      /* first level */
      PrimRefList prims0, prims1;
      PrimInfo   cinfo0, cinfo1;
      FallBackSplit::find(threadIndex,alloc,prims,prims0,cinfo0,prims1,cinfo1);
      
      /* second level */
      PrimRefList cprims[4];
      PrimInfo   cinfo[4];
      FallBackSplit::find(threadIndex,alloc,prims0,cprims[0],cinfo[0],cprims[1],cinfo[1]);
      FallBackSplit::find(threadIndex,alloc,prims1,cprims[2],cinfo[2],cprims[3],cinfo[3]);
      
      /*! create an inner node */
      Node* node = bvh->allocNode(nodeAlloc);
      for (size_t i=0; i<4; i++) 
	if (cinfo[i].size())
	  node->set(i,cinfo[i].geomBounds,createLargeLeaf(threadIndex,nodeAlloc,leafAlloc,cprims[i],cinfo[i],depth+1));

      BVH8::compact(node); // moves empty nodes to the end
      return bvh->encodeNode(node);
    }  

    template<bool PARALLEL>
    const Split BVH8Builder::find(size_t threadIndex, size_t threadCount, size_t depth, PrimRefList& prims, const PrimInfo& pinfo, bool spatial)
    {
      ObjectPartition::SplitInfo oinfo;
      ObjectPartition::Split osplit = ObjectPartition::find<PARALLEL>(threadIndex,threadCount,scheduler,prims,pinfo,logSAHBlockSize,oinfo);
      if (spatial) {
	const BBox3fa overlap = intersect(oinfo.leftBounds,oinfo.rightBounds);
	if (safeArea(overlap) < 0.2f*safeArea(pinfo.geomBounds)) spatial = false;
      }
      if (!spatial) {
	if (osplit.sah == float(inf)) return Split();
	else return osplit;
      }
      SpatialSplit::Split ssplit = SpatialSplit::find<PARALLEL>(threadIndex,threadCount,scheduler,scene,prims,pinfo,logSAHBlockSize);
      const float bestSAH = min(osplit.sah,ssplit.sah);
      
      if      (bestSAH == float(inf)) return Split();
      else if (bestSAH == osplit.sah) return osplit; 
      else if (bestSAH == ssplit.sah) return ssplit;
      else THROW_RUNTIME_ERROR("internal error");      
    }
    
    template<bool PARALLEL>
    __forceinline size_t BVH8Builder::createNode(size_t threadIndex, size_t threadCount, Allocator& nodeAlloc, Allocator& leafAlloc,
                                                 BVH8Builder* parent, BuildRecord& record, BuildRecord records_o[BVH8::N])
    {
      /*! compute leaf and split cost */
      const float leafSAH  = parent->intCost*record.pinfo.leafSAH(parent->logSAHBlockSize);
      const float splitSAH = BVH8::travCost*halfArea(record.pinfo.geomBounds)+parent->intCost*record.split.splitSAH();
      //assert(PrimRefList::block_iterator_unsafe(prims).size() == record.pinfo.size());
      assert(record.pinfo.size() == 0 || leafSAH >= 0 && splitSAH >= 0);
      
      /*! create a leaf node when threshold reached or SAH tells us to stop */
      if (record.pinfo.size() <= parent->minLeafSize || record.depth > BVH8::maxBuildDepth || (record.pinfo.size() <= parent->maxLeafSize && leafSAH <= splitSAH)) {
	*record.dst = parent->createLargeLeaf(threadIndex,nodeAlloc,leafAlloc,record.prims,record.pinfo,record.depth+1); return 0;
      }
      
      /*! initialize child list */
      records_o[0] = record;
      size_t numChildren = 1;
      
      /*! split until node is full or SAH tells us to stop */
      do {
	
	/*! find best child to split */
	float bestSAH = 0; 
	ssize_t bestChild = -1;
	for (size_t i=0; i<numChildren; i++) 
	{
	  float dSAH = records_o[i].split.splitSAH()-records_o[i].pinfo.leafSAH(parent->logSAHBlockSize);
	  if (records_o[i].pinfo.size() <= parent->minLeafSize) continue; 
	  if (records_o[i].pinfo.size() > parent->maxLeafSize) dSAH = min(0.0f,dSAH); //< force split for large jobs
	  if (dSAH <= bestSAH) { bestChild = i; bestSAH = dSAH; }
	}
	if (bestChild == -1) break;
	
	/* perform best found split */
	BuildRecord lrecord(record.depth+1);
	BuildRecord rrecord(record.depth+1);
	records_o[bestChild].split.split<PARALLEL>(threadIndex,threadCount,parent->scheduler,parent->alloc,parent->scene,records_o[bestChild].prims,lrecord.prims,lrecord.pinfo,rrecord.prims,rrecord.pinfo);

	/* fallback if spatial split did fail for corner case */
	if (lrecord.pinfo.size() == 0) {
	  rrecord.split = parent->find<PARALLEL>(threadIndex,threadCount,record.depth,rrecord.prims,rrecord.pinfo,false);
	  records_o[bestChild] = rrecord;
	  continue;
	}

	/* fallback if spatial split did fail for corner case */
	if (rrecord.pinfo.size() == 0) {
	  lrecord.split = parent->find<PARALLEL>(threadIndex,threadCount,record.depth,lrecord.prims,lrecord.pinfo,false);
	  records_o[bestChild] = lrecord;
	  continue;
	}
	
	/* count number of replications caused by spatial splits */
	ssize_t remaining = 0;
	const ssize_t replications = lrecord.pinfo.size()+rrecord.pinfo.size()-records_o[bestChild].pinfo.size(); 
	assert(replications >= 0);
	if (replications >= 0) 
	  remaining = atomic_add(&parent->remainingReplications,-replications);

	/* find new splits */
	lrecord.split = parent->find<PARALLEL>(threadIndex,threadCount,record.depth,lrecord.prims,lrecord.pinfo,parent->enableSpatialSplits && remaining > 0);
	rrecord.split = parent->find<PARALLEL>(threadIndex,threadCount,record.depth,rrecord.prims,rrecord.pinfo,parent->enableSpatialSplits && remaining > 0);
	records_o[bestChild  ] = lrecord;
	records_o[numChildren] = rrecord;
	numChildren++;
	
      } while (numChildren < BVH8::N);
      
      /*! create an inner node */
      Node* node = parent->bvh->allocNode(nodeAlloc);
      for (size_t i=0; i<numChildren; i++) {
	node->set(i,records_o[i].pinfo.geomBounds);
	records_o[i].dst = &node->child(i);
      }
      *record.dst = parent->bvh->encodeNode(node);
      return numChildren;
    }

    void BVH8Builder::finish_build(size_t threadIndex, size_t threadCount, Allocator& nodeAlloc, Allocator& leafAlloc, BuildRecord& record)
    {
      BuildRecord children[BVH8::N]; 
      size_t N = createNode<false>(threadIndex,threadCount,nodeAlloc,leafAlloc,this,record,children);
      for (size_t i=0; i<N; i++)
	finish_build(threadIndex,threadCount,nodeAlloc,leafAlloc,children[i]);
    }

    void BVH8Builder::continue_build(size_t threadIndex, size_t threadCount, Allocator& nodeAlloc, Allocator& leafAlloc, BuildRecord& record)
    {
      /* finish small tasks */
      if (record.pinfo.size() < 4*1024) 
      {
	finish_build(threadIndex,threadCount,nodeAlloc,leafAlloc,record);
#if ROTATE_TREE
	for (int i=0; i<5; i++) 
	  BVH8Rotate::rotate(bvh,*record.dst); 
#endif
	record.dst->setBarrier();
      }

      /* and split large tasks */
      else
      {
	BuildRecord children[BVH8::N];
	size_t N = createNode<false>(threadIndex,threadCount,nodeAlloc,leafAlloc,this,record,children);
      	taskMutex.lock();
	for (size_t i=0; i<N; i++) {
	  tasks.push_back(children[i]);
	  atomic_add(&activeBuildRecords,1);
	}
	taskMutex.unlock();
      }
    }

    void BVH8Builder::build_parallel(size_t threadIndex, size_t threadCount, size_t taskIndex, size_t taskCount) 
    {
      Allocator nodeAlloc(&bvh->alloc);
      Allocator leafAlloc(&bvh->alloc);

      while (activeBuildRecords)
      {
	taskMutex.lock();
	if (tasks.size() == 0) {
	  taskMutex.unlock();
	  continue;
	}
	BuildRecord record = tasks.back();
	tasks.pop_back();
	taskMutex.unlock();
	continue_build(threadIndex,threadCount,nodeAlloc,leafAlloc,record);
	atomic_add(&activeBuildRecords,-1);
      }
      _mm_sfence(); // make written leaves globally visible
    }

    BVH8::NodeRef BVH8Builder::layout_top_nodes(size_t threadIndex, Allocator& nodeAlloc, NodeRef node)
    {
      if (node.isBarrier()) {
	node.clearBarrier();
	return node;
      }
      else if (!node.isLeaf()) 
      {
	Node* src = node.node();
	Node* dst = bvh->allocNode(nodeAlloc);
	for (size_t i=0; i<BVH8::N; i++) {
	  dst->set(i,src->bounds(i),layout_top_nodes(threadIndex,nodeAlloc,src->child(i)));
	}
	return bvh->encodeNode(dst);
      }
      else
	return node;
    }
    
    void BVH8Builder::build(size_t threadIndex, size_t threadCount) 
    {
      //size_t threadCount = min(threadCountOld,getNumberOfCores()); 
      //TaskScheduler::enableThreads(threadCount); // FIXME: enable

      /*! calculate number of primitives */
      size_t numPrimitives = 0;
      if (mesh) numPrimitives = mesh->numTriangles;
      else      numPrimitives = scene->numTriangles;

      /*! set maximal amount of primitive replications for spatial split mode */
      size_t maxPrimitives = numPrimitives;
      if (enableSpatialSplits) {
        maxPrimitives = max(numPrimitives,(size_t)(g_tri_builder_replication_factor*numPrimitives));
	remainingReplications = maxPrimitives-numPrimitives;
      }

      /*! initialize internal buffers of BVH */
      bvh->init(sizeof(BVH8::Node),maxPrimitives,threadCount);

      /*! skip build for empty scene */
      if (numPrimitives == 0) 
	return;
      
      /*! verbose mode */
      if (g_verbose >= 2) {
	std::cout << "building BVH8<" << bvh->primTy.name << "> with " << TOSTRING(isa) "::BVH8Builder(";
	if (enableSpatialSplits) std::cout << "spatialsplits";
	std::cout << ") ... " << std::flush;
      }

      /*! benchmark mode */
      double t0 = 0.0, t1 = 0.0f;
      if (g_verbose >= 2 || g_benchmark)
	t0 = getSeconds();
      
      /* generate list of build primitives */
      PrimRefList prims; PrimInfo pinfo(empty);
      if (mesh) PrimRefListGenFromGeometry<TriangleMesh>::generate(threadIndex,threadCount,scheduler,&alloc,mesh ,prims,pinfo);
      else      PrimRefListGen                          ::generate(threadIndex,threadCount,scheduler,&alloc,scene,TRIANGLE_MESH,1,prims,pinfo);
      
      Allocator nodeAlloc(&bvh->alloc);
      Allocator leafAlloc(&bvh->alloc);

      /* single threaded path */
      if (pinfo.size() <= THRESHOLD_FOR_SINGLE_THREADED)
      {
	const Split split = find<false>(threadIndex,threadCount,1,prims,pinfo,enableSpatialSplits);
	BuildRecord record(1,prims,pinfo,split,&bvh->root);
	finish_build(threadIndex,threadCount,nodeAlloc,leafAlloc,record);
	_mm_sfence(); // make written leaves globally visible
      }

      /* multithreaded path */
      else
      {
	/* perform initial split */
	const Split split = find<true>(threadIndex,threadCount,1,prims,pinfo,enableSpatialSplits);
	BuildRecord record(1,prims,pinfo,split,&bvh->root);
	tasks.push_back(record); 
	activeBuildRecords=1;

	/* work in multithreaded toplevel mode until sufficient subtasks got generated */
	while (tasks.size() > 0 && tasks.size() < threadCount)
	{
	  /* pop largest item for better load balancing */
	  BuildRecord task = tasks.front();
	  std::pop_heap(tasks.begin(),tasks.end());
	  tasks.pop_back();
	  activeBuildRecords--;
	  
	  /* do not generate too small subtasks */
	  if (task.pinfo.size() <= THRESHOLD_FOR_SINGLE_THREADED) {
	    tasks.push_back(task);
	    activeBuildRecords++;
	    break;
	  }
	  
	  /* process this item in parallel */
	  BuildRecord children[BVH8::N];
	  size_t N = createNode<true>(threadIndex,threadCount,nodeAlloc,leafAlloc,this,task,children);
	  for (size_t i=0; i<N; i++) {
	    tasks.push_back(children[i]);
	    std::push_heap(tasks.begin(),tasks.end());
	    activeBuildRecords++;
	  }
	}
	_mm_sfence(); // make written leaves globally visible
	
	/*! process each generated subtask in its own thread */
	scheduler->dispatchTask(threadIndex,threadCount,_build_parallel,this,threadCount,"BVH8Builder::build");

        tasks.clear();
      }

      /* perform tree rotations of top part of the tree */
#if ROTATE_TREE
      for (int i=0; i<5; i++) 
	BVH8Rotate::rotate(bvh,bvh->root);
#endif
      
      /* layout top nodes */
      bvh->root = layout_top_nodes(threadIndex,nodeAlloc,bvh->root);
      //bvh->clearBarrier(bvh->root);
      bvh->numPrimitives = pinfo.size();
      bvh->bounds = pinfo.geomBounds;
      
      /* free all temporary memory blocks */
      Alloc::global.clear();
      //TaskScheduler::enableThreads(threadCountOld); // FIXME: enable
      
      if (g_verbose >= 2 || g_benchmark) 
	t1 = getSeconds();

      /*! verbose mode */
      if (g_verbose >= 2) {
      	std::cout << "[DONE]" << std::endl;
	std::cout << "  dt = " << 1000.0f*(t1-t0) << "ms, perf = " << 1E-6*double(numPrimitives)/(t1-t0) << " Mprim/s" << std::endl;
        std::cout << BVH8Statistics(bvh).str();
      }

      /* benchmark mode */
      if (g_benchmark) {
	BVH8Statistics stat(bvh);
	std::cout << "BENCHMARK_BUILD " << t1-t0 << " " << double(numPrimitives)/(t1-t0) << " " << stat.sah() << " " << stat.bytesUsed() << std::endl;
      }
    }
    
    /*! entry functions for the builder */
    Builder* BVH8Triangle4Builder  (void* bvh, Scene* scene, size_t mode) { return new class BVH8BuilderT<Triangle4> ((BVH8*)bvh,scene,mode); }
    Builder* BVH8Triangle8Builder  (void* bvh, Scene* scene, size_t mode) { return new class BVH8BuilderT<Triangle8> ((BVH8*)bvh,scene,mode); }
  }
}
