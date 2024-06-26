#ifndef __INC_METIN_II_GAME_VECTOR_H__
#define __INC_METIN_II_GAME_VECTOR_H__

typedef struct SVector
{
	float x;
	float y;
	float z;
} VECTOR;

extern void		Normalize(VECTOR * pV1, VECTOR * pV2);
extern float    GetDegreeFromPosition(float x, float y);
extern float    GetDegreeFromPositionXY(int sx, int sy, int ex, int ey);
extern void     GetDeltaByDegree(float fDegree, float fDistance, float *x, float *y);
extern float	GetDegreeDelta(float iDegree, float iDegree2);

#endif
