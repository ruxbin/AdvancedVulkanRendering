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


Camera::Camera(float fov, float n, float f,  vec3 origin, float aspect, vec3 lookat, vec3 up)
{
	_near = n;
	_far = f;
	//mat4 proj = perspective(fov, aspect, n, f);
	mat4 flipx;
	flipx.x[0]=-1;
	flipx.y[1]=1;
	flipx.z[2]=1;
	flipx.w[3]=1;

	mat4 proj = reverseZperspective(fov,aspect,n,f);
	_projectionMatrix = proj*flipx;
	_z = lookat;
	_x = normalize(up.cross(_z));
	_y = _z.cross(_x);
	
	_origin = origin;

	// error C2512 : “Frustum”: 没有合适的默认构造函数可用
	updateCameraMatrix();
}

void Camera::updateCameraMatrix()
{
	//mat4 transM = translate(_origin * -1.f);
	vec3 t(_x.dot(_origin)*-1,_y.dot(_origin)*-1, _z.dot(_origin)*-1);
	_objectToCameraMatrix.x = vec4(_x,t.x);

	_objectToCameraMatrix.y = vec4(_y, t.y);
	_objectToCameraMatrix.z = vec4(_z, t.z);
	_objectToCameraMatrix.w = vec4(0, 0, 0, 1);
	
	_invViewMatrix = inverse(_objectToCameraMatrix);

	//_objectToCameraMatrix = transM * _objectToCameraMatrix;
	//proj to world
	//mat4 viewproj = transpose(_objectToCameraMatrix * _projectionMatrix);
	//mat4 viewproj = transpose(_projectionMatrix)*transpose(_objectToCameraMatrix)*transpose(transM);
	mat4 viewproj = transpose(_projectionMatrix) * transpose(_objectToCameraMatrix);

	mat4 viewprojmatrix = _objectToCameraMatrix* _projectionMatrix;
	_invViewProjectionMatrix = inverse(viewprojmatrix);
	
	mat4 invViewProj = inverse(viewproj);
	vec4 clipCoords[8] = {	{-1,-1,0,1},{-1,1,0,1},{1,1,0,1},{1,-1,0,1},				//far
							{-1,-1,1,1},{-1,1,1,1},{1,1,1,1},{1,-1,1,1} };				//near

	vec4 worldCoordsH[8];
	vec3 worldCoords[8];
	for (int i = 0; i < 8; i++)
	{
		worldCoordsH[i] = invViewProj * clipCoords[i];
		worldCoords[i] = worldCoordsH[i].xyz_w();
	}
	for (int i = 0; i < 4; i++)
		_frustumCorners[i] = worldCoords[i + 4];

	for (int i = 4; i < 8; i++)
		_frustumCorners[i] = worldCoords[i - 4];

	_frustum = { {worldCoords[0],worldCoords[2],worldCoords[1]},{worldCoords[4],worldCoords[5],worldCoords[6]}, //far & near 
				{worldCoords[0],worldCoords[1],worldCoords[4]},{worldCoords[2],worldCoords[3],worldCoords[7]}, //left & right 
				{worldCoords[1],worldCoords[2],worldCoords[5]},{worldCoords[0],worldCoords[4],worldCoords[3]}, //top & bottom 
				};

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
	_y = _z.cross(_x);
	updateCameraMatrix();
}
void Camera::RotateY(float angle)
{
	//rotate _z
	angle = PI * angle / 180.f;
	_x = _x * cosf(angle) + _z * sinf(angle);
	_z = _x.cross(_y);
	updateCameraMatrix();
}
