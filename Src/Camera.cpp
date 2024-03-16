#include "Camera.h"

void* Camera::getProjectionMatrixData()
{
    return _projectionMatrix.value_ptr();
}

void* Camera::getObjectToCameraData()
{
    return _objectToCameraMatrix.value_ptr();
}

mat4& Camera::getObjectToCamera()
{
    return _objectToCameraMatrix;
}

mat4& Camera::getProjectMatrix()
{
    return _projectionMatrix;
}

Camera::Camera(float fov,float n ,float f, vec3 origin,float aspect)
{
    mat4 trans = translate(-origin.x,-origin.y,-origin.z);
    mat4 proj = perspective(fov, aspect, n, f);
    _projectionMatrix = proj;
    _objectToCameraMatrix = trans;
	_origin = origin;
	_z = vec3(0,0,1);
	_x = vec3(1,0,0);
	_y = vec3(0,1,0);
}

void Camera::MoveLeft(float dist)
{
	mat4 trans = translate(_x*-dist);
	_objectToCameraMatrix = trans * _objectToCameraMatrix;

}

void Camera::MoveRight(float dist)
{
	mat4 trans = translate(_x*dist);
	_objectToCameraMatrix = trans * _objectToCameraMatrix;

}

void Camera::MoveUp(float dist)
{
	mat4 trans = translate(_y*-dist);
	_objectToCameraMatrix = trans * _objectToCameraMatrix;

}

void Camera::MoveDown(float dist)
{
	mat4 trans = translate(_y*dist);
	_objectToCameraMatrix = trans * _objectToCameraMatrix;

}

void Camera::MoveForward(float dist)
{
	mat4 trans = translate(_z * -dist);
	_objectToCameraMatrix = trans * _objectToCameraMatrix;

}

void Camera::MoveBackward(float dist)
{
	mat4 trans = translate(_z * dist);
	_objectToCameraMatrix = trans * _objectToCameraMatrix;
}
