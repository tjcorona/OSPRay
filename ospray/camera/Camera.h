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
#include "ospray/common/Ray.h"

namespace ospray {
  
  //! base camera class abstraction 
  /*! the base class itself does not do anything useful; look into
      perspectivecamera etc for that */
  struct Camera : public ManagedObject {
    //! \brief common function to help printf-debugging 
    /*! Every derived class should overrride this! */
    virtual std::string toString() const { return "ospray::Camera (base class)"; }
    static Camera *createCamera(const char *identifier);
    virtual void initRay(Ray &ray, const vec2f &sample) = 0;
  };

  /*! \brief registers a internal ospray::'ClassName' camera under
      the externally accessible name "external_name" 
      
      \internal This currently works by defining a extern "C" function
      with a given predefined name that creates a new instance of this
      camera. By having this symbol in the shared lib ospray can
      lateron always get a handle to this fct and create an instance
      of this camera.
  */
#define OSP_REGISTER_CAMERA(InternalClassName,external_name)        \
  extern "C" Camera *ospray_create_camera__##external_name()        \
  {                                                                 \
    return new InternalClassName;                                   \
  }                                                                 \

} // ::ospray
