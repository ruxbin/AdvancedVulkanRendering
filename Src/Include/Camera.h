#pragma once
#include "Matrix.h"
#include "Frustum.h"
class Camera
{
private:
    mat4 _projectionMatrix;
    mat4 _objectToCameraMatrix;
    mat4 _invViewMatrix;
    mat4 _invViewProjectionMatrix;
    Camera();
    vec3 _origin;
    vec3 _x;//left & right -- a/d
    vec3 _y;//up & down -- w/s
    vec3 _z;//forward & backword --f/g
    void updateCameraMatrix();
    Frustum _frustum;
public:
   
    Camera(float fov, float n, float f, vec3 origin, float aspect, vec3 lookat, vec3 right);
    void * getProjectionMatrixData();
    void * getObjectToCameraData();
    mat4 & getObjectToCamera();
    mat4 & getProjectMatrix();
    mat4 & getInvViewMatrix(){return _invViewMatrix;}
    mat4& getInvViewProjectionMatrix() { return _invViewProjectionMatrix; }
    void MoveLeft(float length);
    void MoveRight(float length);
    void MoveForward(float length);
    void MoveBackward(float length);
    void MoveUp(float length);
    void MoveDown(float length);
    void RotateZ(float angle);
    void RotateY(float angle);
    const Frustum& getFrustum()const { return _frustum; }

};

