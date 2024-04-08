#include "Camera.h"
#include <math.h>

#ifndef PI
#define PI 3.141592653f
#endif

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

Camera::Camera(float fov, float n, float f,  vec3 origin, float aspect, vec3 lookat, vec3 right)
{
	
	mat4 proj = perspective(fov, aspect, n, f);
	_projectionMatrix = proj;
	vec3 up = right.cross(lookat);
	_x = right;
	_y = up;
	_z = lookat;
	_origin = origin;

	_objectToCameraMatrix.x = vec4(_x, -_origin.x);
	
	_objectToCameraMatrix.y = vec4(_y,-_origin.y);
	_objectToCameraMatrix.z = vec4(_z, -_origin.z);
	_objectToCameraMatrix.w = vec4(0, 0, 0, 1);
}

void Camera::updateCameraMatrix()
{
	_objectToCameraMatrix.x = vec4(_x, -_origin.x);

	_objectToCameraMatrix.y = vec4(_y, -_origin.y);
	_objectToCameraMatrix.z = vec4(_z, -_origin.z);
	_objectToCameraMatrix.w = vec4(0, 0, 0, 1);
}

void Camera::MoveLeft(float dist)
{
	_origin -= _x*dist;
	updateCameraMatrix();

}

void Camera::MoveRight(float dist)
{
	
	_origin += _x * dist;
	updateCameraMatrix();
}

void Camera::MoveUp(float dist)
{
	_origin -= _y * dist;
	updateCameraMatrix();
}

void Camera::MoveDown(float dist)
{
	
	_origin += _y * dist;
	updateCameraMatrix();
}

void Camera::MoveForward(float dist)
{
	_origin += _z * dist;
	updateCameraMatrix();
}

void Camera::MoveBackward(float dist)
{
	_origin -= _z * dist;
	updateCameraMatrix();
}

void Camera::RotateZ(float angle)
{
	//rotate _x
	angle = PI * angle / 180.f;
	_x = _x*cosf(angle)+_y*sinf(angle);
	_y = _x.cross(_z);
	updateCameraMatrix();
}
void Camera::RotateY(float angle)
{
	//rotate _z
	angle = PI * angle / 180.f;
	_x = _x * cosf(angle) + _z * sinf(angle);
	_z = _y.cross(_x);
	updateCameraMatrix();
}