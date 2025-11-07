#include "MarchingCubeGen.h"
#include "FastNoiseLite.h"
#include "ProceduralMeshComponent.h"


// Sets default values
AMarchingCubeGen::AMarchingCubeGen()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	mesh = CreateDefaultSubobject<UProceduralMeshComponent>("Mesh");
	noise = new FastNoiseLite();

	mesh->SetCastShadow(false);

	// Set Mesh as root
	SetRootComponent(mesh);
}

AMarchingCubeGen::~AMarchingCubeGen()
{
	delete noise;
}


// Called when the game starts or when spawned
void AMarchingCubeGen::BeginPlay()
{
	Super::BeginPlay();
    noise->SetFrequency(frequency);
    noise->SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    noise->SetFractalType(FastNoiseLite::FractalType_FBm);

    Setup();

    FVector Position = GetActorLocation() / 100;

    Async(EAsyncExecution::ThreadPool, [this, Position]()
    {
        // Step 1: Generate voxel data
        GenerateHeightMap(Position);

        // Step 2: Split Z-axis into sections for parallelism
        int sectionCount = FMath::Max(1, FPlatformMisc::NumberOfCores() / 2);
        TArray<TFuture<FThreadMeshData>> futures;

        for (int s = 0; s < sectionCount; ++s)
        {
            int zStart = s * size / sectionCount;
            int zEnd = (s + 1) * size / sectionCount;

            futures.Add(Async(EAsyncExecution::ThreadPool, [this, zStart, zEnd]()
            {
                FThreadMeshData threadData;
                threadData.Reset();
                GenerateMesh(zStart, zEnd, threadData);
                return threadData;
            }));
        }

        // Step 3: Merge results on the game thread
        AsyncTask(ENamedThreads::GameThread, [this, futures = MoveTemp(futures)]() mutable
        {
            meshData.Clear();
            vertexCount = 0;

            for (int i = 0; i < futures.Num(); ++i)
            {
                FThreadMeshData td = futures[i].Get();

                // Correct triangle indexing by offsetting
                int32 baseVertex = meshData.Vertices.Num();

                for (int32 t = 0; t < td.Triangles.Num(); ++t)
                {
                    meshData.Triangles.Add(td.Triangles[t] + baseVertex);
                }

                meshData.Vertices.Append(td.Vertices);
                meshData.Normals.Append(td.Normals);
                meshData.Colors.Append(td.Colors);
            }

            vertexCount = meshData.Vertices.Num();

            ApplyMesh();
        });
    });
}

void AMarchingCubeGen::Setup()
{
	Voxels.SetNum((size + 1) * (size + 1) * (size + 1));
}

void AMarchingCubeGen::GenerateHeightMap(const FVector position)
{
	for (int x = 0; x <= size; ++x)
	{
		for (int y = 0; y <= size; ++y)
		{
			for (int z = 0; z <= size; ++z)
			{
				Voxels[GetVoxelIndex(x,y,z)] = noise->GetNoise(
					x + position.X, 
					y + position.Y, 
					z + position.Z
				);	
			}
		}
	}
}

void const AMarchingCubeGen::GenerateMesh(int zStart, int zEnd,FThreadMeshData& data)
{
	if (surfaceLevel > 0.0f)
	{
		TriangleOrder[0] = 0;
		TriangleOrder[1] = 1;
		TriangleOrder[2] = 2;
	}
	else
	{
		TriangleOrder[0] = 2;
		TriangleOrder[1] = 1;
		TriangleOrder[2] = 0;
	}
	float Cube[8];
	for (int X = 0; X < size; ++X)
	{
		for (int Y = 0; Y < size; ++Y)
		{
			for (int Z = zStart; Z < zEnd; ++Z)
			{
				for (int i = 0; i < 8; ++i)
				{
					int VX = X + VertexOffset[i][0];
					int VY = Y + VertexOffset[i][1];
					int VZ = Z + VertexOffset[i][2];

					Cube[i] = GetVoxelDensityWithModif(VX, VY, VZ);
				}
				March(X, Y, Z, Cube,data);
			}
		}
	}
}

void AMarchingCubeGen::March(int X, int Y, int Z, const float Cube[8],FThreadMeshData& data)
{
	int VertexMask = 0;
    FVector EdgeVertex[12];

    for (int i = 0; i < 8; ++i)
    {
        if (Cube[i] <= surfaceLevel)
        {
        	VertexMask |= 1 << i;  
        } 
    }

    const int EdgeMask = CubeEdgeFlags[VertexMask];
    if (EdgeMask == 0) return;

    // Compute intersection points
    for (int i = 0; i < 12; ++i)
    {
        if ((EdgeMask & (1 << i)) != 0)
        {
            float offset = GetInterpolationOffset(Cube[EdgeConnection[i][0]], Cube[EdgeConnection[i][1]]);
            EdgeVertex[i].X = X + VertexOffset[EdgeConnection[i][0]][0] + offset * EdgeDirection[i][0];
            EdgeVertex[i].Y = Y + VertexOffset[EdgeConnection[i][0]][1] + offset * EdgeDirection[i][1];
            EdgeVertex[i].Z = Z + VertexOffset[EdgeConnection[i][0]][2] + offset * EdgeDirection[i][2];
        }
    }

    for (int i = 0; i < 5; ++i)
    {
        if (TriangleConnectionTable[VertexMask][3*i] < 0) break;

        auto V1 = EdgeVertex[TriangleConnectionTable[VertexMask][3*i]] * 100;
        auto V2 = EdgeVertex[TriangleConnectionTable[VertexMask][3*i + 1]] * 100;
        auto V3 = EdgeVertex[TriangleConnectionTable[VertexMask][3*i + 2]] * 100;

        auto Normal = FVector::CrossProduct(V2 - V1, V3 - V1);
        if (!Normal.Normalize()) Normal = FVector::UpVector;
        Normal.Normalize();
        auto Color = FColor::MakeRandomColor();

        data.Vertices.Append({V1, V2, V3});
        data.Triangles.Append({ data.VertexCount + TriangleOrder[0],
                                data.VertexCount + TriangleOrder[1],
                                data.VertexCount + TriangleOrder[2] });
        data.Normals.Append({Normal, Normal, Normal});
        data.Colors.Append({Color, Color, Color});
        data.VertexCount += 3;
    }
}

int AMarchingCubeGen::GetVoxelIndex(int X, int Y, int Z) const
{
	return Z * (size + 1) * (size + 1) + Y * (size + 1) + X;
}

float AMarchingCubeGen::GetInterpolationOffset(float V1, float V2) const
{
	const float delta = V2 - V1;
	if (fabsf(delta) < 1e-6f)
		return 0.5f;

	float t = (surfaceLevel - V1) / delta;
	return FMath::Clamp(t, 0.0f, 1.0f);
}

void AMarchingCubeGen::ApplyMesh() 
{
	
	TMap<FIntVector, int32> vertexLookup;


    const float precision = 0.001f; 


    TArray<FVector> finalVertices;
    TArray<FVector> finalNormals;
    TArray<FColor>  finalColors;
    TArray<int32>   finalTriangles;
	
    auto Quantize = [&](const FVector& v)
    {
        return FIntVector(
            FMath::RoundToInt(v.X / precision),
            FMath::RoundToInt(v.Y / precision),
            FMath::RoundToInt(v.Z / precision)
        );
    };

    // Returns existing vertex index or creates a new one
    auto GetOrAddVertex = [&](const FVector& v, const FVector& n, const FColor& c)
    {
        FIntVector key = Quantize(v);
        if (int32* existingIndex = vertexLookup.Find(key))
        {
            // Accumulate normals for smooth shading
            finalNormals[*existingIndex] += n;
            return *existingIndex;
        }

        int32 newIndex = finalVertices.Num();
        vertexLookup.Add(key, newIndex);
        finalVertices.Add(v);
        finalNormals.Add(n);
        finalColors.Add(c);
        return newIndex;
    };

    // Rebuild triangles with deduplication
	for (int32 i = 0; i < meshData.Triangles.Num(); i += 3)
	{
		int32 i1 = GetOrAddVertex(
			meshData.Vertices[meshData.Triangles[i]],
			meshData.Normals[meshData.Triangles[i]],
			meshData.Colors[meshData.Triangles[i]]
		);
		int32 i2 = GetOrAddVertex(
			meshData.Vertices[meshData.Triangles[i + 1]],
			meshData.Normals[meshData.Triangles[i + 1]],
			meshData.Colors[meshData.Triangles[i + 1]]
		);
		int32 i3 = GetOrAddVertex(
			meshData.Vertices[meshData.Triangles[i + 2]],
			meshData.Normals[meshData.Triangles[i + 2]],
			meshData.Colors[meshData.Triangles[i + 2]]
		);

		// Skip degenerate triangles
		if (i1 != i2 && i2 != i3 && i3 != i1)
		{
			finalTriangles.Append({i1, i2, i3});
		}
	}

    // Normalize accumulated normals
    for (FVector& N : finalNormals)
    {
        N.Normalize();
    }


    mesh->SetMaterial(0, material);
    mesh->CreateMeshSection(
        0,
        finalVertices,
        finalTriangles,
        finalNormals,
        meshData.UV0,     
        finalColors,
        TArray<FProcMeshTangent>(),
        true
    );
}

float AMarchingCubeGen::GetVoxelDensityWithModif(int X, int Y, int Z) const
{
	FIntVector Key(X, Y, Z);
	
	float BaseDensity = Voxels[GetVoxelIndex(X, Y, Z)];
	
	if (const float* Delta = modifications.Find(Key))
	{
		BaseDensity += *Delta;
	}

	return BaseDensity;
}


void AMarchingCubeGen::ModifyVoxel(const FVector& worldPos, float editingSpeed, float brushRadius)
{
    FVector Local = (worldPos - GetActorLocation()) / 100.0f;

    int minX = FMath::FloorToInt(Local.X - brushRadius);
    int maxX = FMath::CeilToInt(Local.X + brushRadius);
    int minY = FMath::FloorToInt(Local.Y - brushRadius);
    int maxY = FMath::CeilToInt(Local.Y + brushRadius);
    int minZ = FMath::FloorToInt(Local.Z - brushRadius);
    int maxZ = FMath::CeilToInt(Local.Z + brushRadius);

    for (int x = minX; x <= maxX; x++)
    {
        for (int y = minY; y <= maxY; y++)
        {
            for (int z = minZ; z <= maxZ; z++)
            {
                FVector voxelCenter(x + 0.5f, y + 0.5f, z + 0.5f);
                float dist = FVector::Dist(voxelCenter, Local);

                if (dist < brushRadius)
                {
                    // Linear falloff (your formula)
                    float falloff = 1.0f - (dist / brushRadius);

                    // Density change per tick (smoother editing)
                    float deltaDensity = falloff * editingSpeed * GetWorld()->DeltaTimeSeconds;

                    FIntVector voxelIndex(x, y, z);
                    float& currentDensity = modifications.FindOrAdd(voxelIndex, 0.0f);

                    // Apply density modification (raising or lowering terrain)
                    currentDensity += deltaDensity;
                }
            }
        }
    }

	SaveModifications();
    // Async mesh rebuild
    TFuture<FThreadMeshData> Future = Async(EAsyncExecution::ThreadPool, [this]() -> FThreadMeshData
    {
        FThreadMeshData ThreadData;
        ThreadData.Reset();
        GenerateMesh(0, size, ThreadData);
        return ThreadData;
    });

    AsyncTask(ENamedThreads::GameThread, [this, Future = MoveTemp(Future)]() mutable
    {
        FThreadMeshData Result = Future.Get();

        meshData.Clear();
        meshData.Vertices = MoveTemp(Result.Vertices);
        meshData.Triangles = MoveTemp(Result.Triangles);
        meshData.Normals = MoveTemp(Result.Normals);
        meshData.Colors = MoveTemp(Result.Colors);
        meshData.VertexCount = Result.VertexCount;
    	
        ApplyMesh();
    });
}

void AMarchingCubeGen::SaveModifications()
{
	if (modifications.Num() == 0)
		return; // nothing to save

	// Capture current state safely
	TMap<FIntVector, float> ModCopy = modifications;

	FString SaveDir = FPaths::ProjectSavedDir() / TEXT("VoxelChunks");
	IFileManager::Get().MakeDirectory(*SaveDir, true);

	FIntVector ChunkCoord(
		FMath::FloorToInt(GetActorLocation().X / (size * 100)),
		FMath::FloorToInt(GetActorLocation().Y / (size * 100)),
		FMath::FloorToInt(GetActorLocation().Z / (size * 100))
	);

	FString FileName = FString::Printf(TEXT("%s/Chunk_%d_%d_%d.sav"),
		*SaveDir, ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);

	// Launch async file save
	Async(EAsyncExecution::ThreadPool, [FileName, ModCopy = MoveTemp(ModCopy)]()
	{
		FString SaveData;
		SaveData.Reserve(ModCopy.Num() * 32); // pre-allocate for efficiency

		for (const auto& Pair : ModCopy)
		{
			SaveData += FString::Printf(TEXT("%d,%d,%d,%f\n"),
				Pair.Key.X, Pair.Key.Y, Pair.Key.Z, Pair.Value);
		}

		FFileHelper::SaveStringToFile(SaveData, *FileName);
	});
}

void AMarchingCubeGen::LoadModifications()
{
	FString SaveDir = FPaths::ProjectSavedDir() / TEXT("VoxelChunks");

	FIntVector ChunkCoord(
		FMath::FloorToInt(GetActorLocation().X / (size * 100)),
		FMath::FloorToInt(GetActorLocation().Y / (size * 100)),
		FMath::FloorToInt(GetActorLocation().Z / (size * 100))
	);

	FString FileName = FString::Printf(TEXT("%s/Chunk_%d_%d_%d.sav"),
		*SaveDir, ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);

	if (!FPaths::FileExists(FileName))
		return;

	FString LoadedData;
	FFileHelper::LoadFileToString(LoadedData, *FileName);

	TArray<FString> Lines;
	LoadedData.ParseIntoArrayLines(Lines);

	for (FString& Line : Lines)
	{
		TArray<FString> Parts;
		Line.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 4) continue;

		FIntVector Pos(
			FCString::Atoi(*Parts[0]),
			FCString::Atoi(*Parts[1]),
			FCString::Atoi(*Parts[2])
		);
		float Density = FCString::Atof(*Parts[3]);
		modifications.Add(Pos, Density);
	}
}

