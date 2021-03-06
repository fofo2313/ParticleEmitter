//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Common.hlsli"

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
cbuffer cbSimulation
{
	uint	g_numParticles;
	float	g_smoothRadius;
	float	g_pressureStiffness;
	float	g_restDensity;
	float	g_densityCoef;
	float	g_pressureGradCoef;
	float	g_viscosityLaplaceCoef;
};

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
static const float g_hSq = g_smoothRadius * g_smoothRadius;

//--------------------------------------------------------------------------------------
// Buffers
//--------------------------------------------------------------------------------------
StructuredBuffer<Particle> g_roParticles;
Buffer<uint>	g_roGrid;
