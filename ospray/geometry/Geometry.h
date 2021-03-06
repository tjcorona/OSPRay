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

#pragma once

#include "ospray/common/Managed.h"
#include "ospray/common/OSPCommon.h"
#include "ospray/common/Material.h"

namespace ospray {

  struct Model;

  /*! \brief abstract base class for geometries.

    Geometries are container objects that store primitives (like
    triangles); multiple geometries together can form a model, which
    is generally a set of geometries that logically go together (and
    that usually share the same acceleration structures). I.e., in the
    context of polygonal geometry a polygonal model might be specified
    as a set of individual triangle meshes that each have a different
    material (and their own connectivity and vertex sharing info), but
    that together form a single model with a single accel structure */
  struct Geometry : public ManagedObject
  {
    //! constructor
    Geometry() : bounds(embree::empty) { managedObjectType = OSP_GEOMETRY; }

    //! set given geometry's material. 
    /*! all material assignations should go through this function; the
        'material' field itself is private). This allows the
        respective geometry's derived instance to always properly set
        the material field of the ISCP-equivalent whenever the
        c++-side's material gets changed */
    virtual void setMaterial(Material *mat);

    //! get material assigned to this geometry 
    virtual Material *getMaterial() const { return material.ptr; };

    //! \brief common function to help printf-debugging 
    virtual std::string toString() const { return "ospray::Geometry"; }

    /*! \brief integrates this geometry's primitives into the respective
        model's acceleration structure */
    virtual void finalize(Model *model) {}

    /*! \brief creates an abstract geometry class of given type 

      The respective geometry type must be a registered geometry type
      in either ospray proper or any already loaded module. For
      geometry types specified in special modules, make sure to call
      ospLoadModule first. */
    static Geometry *createGeometry(const char *type); 

    box3f bounds;

  private:
    //! material associated to this geometry
    /*! this field is private to make sure it is only set through
        'setMaterial' (see comment there) */
    Ref<Material> material;
  };

  /*! \brief registers a internal ospray::<ClassName> geometry under
      the externally accessible name "external_name" 
      
      \internal This currently works by defining a extern "C" function
      with a given predefined name that creates a new instance of this
      geometry. By having this symbol in the shared lib ospray can
      lateron always get a handle to this fct and create an instance
      of this geometry.
  */
#define OSP_REGISTER_GEOMETRY(InternalClassName,external_name)      \
  extern "C" ospray::Geometry *ospray_create_geometry__##external_name() \
  {                                                                 \
    return new InternalClassName;                                   \
  }                                                                 \
  
} // ::ospray
