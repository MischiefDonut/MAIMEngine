/*
** hw_levelmesh.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include <memory>
#include "tarray.h"
#include "vectors.h"
#include "hw_collision.h"

namespace hwrenderer
{

class LevelMesh
{
public:
	virtual ~LevelMesh() = default;

	TArray<FVector3> MeshVertices;
	TArray<int> MeshUVIndex;
	TArray<uint32_t> MeshElements;
	TArray<int> MeshSurfaces;

	std::unique_ptr<TriangleMeshShape> Collision;

	bool Trace(const FVector3& start, FVector3 direction, float maxDist)
	{
		FVector3 end = start + direction * (std::max)(maxDist - 10.0f, 0.0f);
		return !TriangleMeshShape::find_any_hit(Collision.get(), start, end);
	}
};

} // namespace
