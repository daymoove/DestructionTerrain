#pragma once

#include "CoreMinimal.h"
#include "MeshData.generated.h"

USTRUCT()
struct FMeshData
{
	GENERATED_BODY()
	TArray<FVector> Vertices;
	TArray<int> Triangles;
	TArray<FVector> Normals;
	TArray<FColor> Colors;
	TArray<FVector2D> UV0;

	void Clear();
};

inline void FMeshData::Clear()
{
	Vertices.Empty();
	Triangles.Empty();
	Normals.Empty();
	Colors.Empty();
	UV0.Empty();
}