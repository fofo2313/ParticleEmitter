//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
const float g_densityCoef;

//--------------------------------------------------------------------------------------
// Buffers
//--------------------------------------------------------------------------------------
RWTexture3D<float>	g_rwDensity;
Texture3D<uint>		g_roGrid;

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const uint numParticlesPerCell = g_roGrid[DTid];

	g_rwDensity[DTid] = g_densityCoef * numParticlesPerCell;
}
