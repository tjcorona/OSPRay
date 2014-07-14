#undef NDEGBUG

// ospray
#include "instance.h"
#include "ospray/common/model.h"
// ispc-generated files
#include "instance_ispc.h"

namespace ospray {

  Instance::Instance()
  {
    this->ispcEquivalent = ispc::InstanceGeometry_create(this);
  }

  void Instance::finalize(Model *model) 
  {
    xfm.l.vx = getParam3f("xfm.l.vx",vec3f(1.f,0.f,0.f));
    xfm.l.vy = getParam3f("xfm.l.vy",vec3f(0.f,1.f,0.f));
    xfm.l.vz = getParam3f("xfm.l.vz",vec3f(0.f,0.f,1.f));
    xfm.p   = getParam3f("xfm.p",vec3f(0.f,0.f,0.f));

    instancedScene = (Model *)getParamObject("model",NULL);
    
    embreeGeomID = rtcNewInstance(model->embreeSceneHandle,
                                  instancedScene->embreeSceneHandle);
    rtcSetTransform(model->embreeSceneHandle,embreeGeomID,
                    RTC_MATRIX_COLUMN_MAJOR,
                    (const float *)&xfm);
    rtcEnable(model->embreeSceneHandle,embreeGeomID);
    AffineSpace3f rcp_xfm = rcp(xfm);
    ispc::InstanceGeometry_set(getIE(),
                               (ispc::AffineSpace3f&)xfm,
                               (ispc::AffineSpace3f&)rcp_xfm,
                               instancedScene->getIE());
  }

  OSP_REGISTER_GEOMETRY(Instance,instance);
}