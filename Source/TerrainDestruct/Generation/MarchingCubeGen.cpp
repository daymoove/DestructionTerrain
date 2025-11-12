#include "MarchingCubeGen.h"
#include "TerrainDestruct/Utils/FastNoiseLite.h"
#include "ProceduralMeshComponent.h"

// Constructor for the Marching Cubes terrain generation actor
AMarchingCubeGen::AMarchingCubeGen()
{
	// Enable tick for frame updates
	PrimaryActorTick.bCanEverTick = true;
	
	// Create the procedural mesh component
	mesh = CreateDefaultSubobject<UProceduralMeshComponent>("Mesh");
	
	// Initialize the Perlin noise generator
	noise = new FastNoiseLite();

	// Disable shadow casting for performance optimization
	mesh->SetCastShadow(false);

	// Set the mesh as the root component
	SetRootComponent(mesh);
}

// Destructor - clean up dynamically allocated noise generator
AMarchingCubeGen::~AMarchingCubeGen()
{
	delete noise;
}



// Called when the actor is spawned - initializes noise and generates the initial mesh
void AMarchingCubeGen::BeginPlay()
{
	Super::BeginPlay();
	
	// Configure the noise generator with specified parameters
    noise->SetFrequency(frequency);
    noise->SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    noise->SetFractalType(FastNoiseLite::FractalType_FBm);

	// Initialize the voxel grid
    Setup();

	// Get the chunk's world position converted to local coordinates
    FVector Position = GetActorLocation() / 100;

	// Generate mesh asynchronously on thread pool to avoid blocking the game thread
    Async(EAsyncExecution::ThreadPool, [this, Position]()
    {
		// Generate the height map (voxel density values) using Perlin noise
        GenerateHeightMap(Position);

		// Calculate number of sections to divide work across multiple CPU cores
        int sectionCount = FMath::Max(1, FPlatformMisc::NumberOfCores() / 2);
        TArray<TFuture<FThreadMeshData>> futures;

		// Create async tasks for each section of the mesh
        for (int s = 0; s < sectionCount; ++s)
        {
            int zStart = s * size / sectionCount;
            int zEnd = (s + 1) * size / sectionCount;

			// Queue mesh generation for this section
            futures.Add(Async(EAsyncExecution::ThreadPool, [this, zStart, zEnd]()
            {
                FThreadMeshData threadData;
                threadData.Reset();
                GenerateMesh(zStart, zEnd, threadData);
                return threadData;
            }));
        }

		// Once all sections are generated, combine them on the game thread
        AsyncTask(ENamedThreads::GameThread, [this, futures = MoveTemp(futures)]() mutable
        {
			// Clear previous mesh data
            meshData.Clear();
            vertexCount = 0;

			// Combine all mesh sections into a single mesh
            for (int i = 0; i < futures.Num(); ++i)
            {
                FThreadMeshData td = futures[i].Get();

				// Offset triangle indices to account for new vertex base
                int32 baseVertex = meshData.Vertices.Num();

                for (int32 t = 0; t < td.Triangles.Num(); ++t)
                {
                    meshData.Triangles.Add(td.Triangles[t] + baseVertex);
                }

				// Append vertices, normals, and colors from this section
                meshData.Vertices.Append(td.Vertices);
                meshData.Normals.Append(td.Normals);
                meshData.Colors.Append(td.Colors);
            }

			// Track total vertex count
            vertexCount = meshData.Vertices.Num();

			// Apply the combined mesh to the procedural mesh component
            ApplyMesh();
        });
    });
}

// Initialize the voxel grid with the specified size
void AMarchingCubeGen::Setup()
{
	// Allocate memory for (size+1)^3 voxels to store density values
	Voxels.SetNum((size + 1) * (size + 1) * (size + 1));
}

// Generate voxel density values using Perlin noise for terrain surface
void AMarchingCubeGen::GenerateHeightMap(const FVector position)
{
	// Iterate through all voxel positions in the grid
	for (int x = 0; x <= size; ++x)
	{
		for (int y = 0; y <= size; ++y)
		{
			for (int z = 0; z <= size; ++z)
			{
				// Sample noise at this position and store as voxel density
				Voxels[GetVoxelIndex(x,y,z)] = noise->GetNoise(
					x + position.X, 
					y + position.Y, 
					z + position.Z
				);	
			}
		}
	}
}

// Generate mesh geometry using marching cubes algorithm for a Z-range section
void AMarchingCubeGen::GenerateMesh(int zStart, int zEnd,FThreadMeshData& data)
{
	// Set triangle winding order based on surface level sign
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

	// Array to store the 8 corner density values of the current cube
	float Cube[8];

	// Iterate through all cube cells in the voxel grid for the specified Z-range
	for (int X = 0; X < size; ++X)
	{
		for (int Y = 0; Y < size; ++Y)
		{
			for (int Z = zStart; Z < zEnd; ++Z)
			{
				// Gather the 8 corner voxel densities for this cube
				for (int i = 0; i < 8; ++i)
				{
					int VX = X + VertexOffset[i][0];
					int VY = Y + VertexOffset[i][1];
					int VZ = Z + VertexOffset[i][2];

					Cube[i] = GetVoxelDensityWithModif(VX, VY, VZ);
				}

				// Process this cube with the marching cubes algorithm
				March(X, Y, Z, Cube, data);
			}
		}
	}
}

// Process a single cube using the marching cubes algorithm to create triangles
void AMarchingCubeGen::March(int X, int Y, int Z, const float Cube[8],FThreadMeshData& data)
{
	// Determine which corners are below the surface level using a bitmask
	int VertexMask = 0;
    FVector EdgeVertex[12];

    for (int i = 0; i < 8; ++i)
    {
        if (Cube[i] <= surfaceLevel)
        {
        	VertexMask |= 1 << i;  
        } 
    }

	// Look up which edges of the cube intersect the surface
    const int EdgeMask = CubeEdgeFlags[VertexMask];
    if (EdgeMask == 0) return;  // No triangles for this cube

	// Calculate intersection points along the cube edges that cross the surface
    for (int i = 0; i < 12; ++i)
    {
        if ((EdgeMask & (1 << i)) != 0)
        {
			// Interpolate to find the exact surface crossing point
            float offset = GetInterpolationOffset(Cube[EdgeConnection[i][0]], Cube[EdgeConnection[i][1]]);
            EdgeVertex[i].X = X + VertexOffset[EdgeConnection[i][0]][0] + offset * EdgeDirection[i][0];
            EdgeVertex[i].Y = Y + VertexOffset[EdgeConnection[i][0]][1] + offset * EdgeDirection[i][1];
            EdgeVertex[i].Z = Z + VertexOffset[EdgeConnection[i][0]][2] + offset * EdgeDirection[i][2];
        }
    }

	// Generate up to 5 triangles for this cube based on surface configuration
    for (int i = 0; i < 5; ++i)
    {
        if (TriangleConnectionTable[VertexMask][3*i] < 0) break;

		// Get the three vertices of this triangle in world space (multiply by 100 for UE units)
        auto V1 = EdgeVertex[TriangleConnectionTable[VertexMask][3*i]] * 100;
        auto V2 = EdgeVertex[TriangleConnectionTable[VertexMask][3*i + 1]] * 100;
        auto V3 = EdgeVertex[TriangleConnectionTable[VertexMask][3*i + 2]] * 100;

		// Calculate surface normal from cross product of triangle edges
        auto Normal = FVector::CrossProduct(V2 - V1, V3 - V1);
        if (!Normal.Normalize()) Normal = FVector::UpVector;
        Normal.Normalize();

		// Assign a random color to each triangle (for debugging)
        auto Color = FColor::MakeRandomColor();

		// Add the three vertices to the mesh data
        data.Vertices.Append({V1, V2, V3});

		// Add triangle indices with proper winding order
        data.Triangles.Append({ data.VertexCount + TriangleOrder[0],
                                data.VertexCount + TriangleOrder[1],
                                data.VertexCount + TriangleOrder[2] });

		// Add normals and colors for each vertex
        data.Normals.Append({Normal, Normal, Normal});
        data.Colors.Append({Color, Color, Color});
        data.VertexCount += 3;
    }
}

// Convert 3D voxel coordinates to a 1D array index
int AMarchingCubeGen::GetVoxelIndex(int X, int Y, int Z) const
{
	return Z * (size + 1) * (size + 1) + Y * (size + 1) + X;
}

// Calculate linear interpolation offset between two density values to find surface crossing point
float AMarchingCubeGen::GetInterpolationOffset(float V1, float V2) const
{
	const float delta = V2 - V1;
	// Avoid division by zero
	if (fabsf(delta) < 1e-6f)
		return 0.5f;

	// Clamp interpolation to valid range [0, 1]
	float t = (surfaceLevel - V1) / delta;
	return FMath::Clamp(t, 0.0f, 1.0f);
}


// Apply generated mesh data to the procedural mesh component with vertex deduplication
void AMarchingCubeGen::ApplyMesh()
{
	// Map to track duplicate vertices based on quantized position
	TMap<FIntVector, int32> vertexLookup;

	// Precision threshold for vertex merging (prevents duplicate vertices with slight position variations)
    const float precision = 0.001f; 

	// Final mesh data with deduplicated vertices
    TArray<FVector> finalVertices;
    TArray<FVector> finalNormals;
    TArray<FColor>  finalColors;
    TArray<int32>   finalTriangles;
	
	// Quantize vertex position to a grid for duplicate detection
    auto Quantize = [&](const FVector& v)
    {
        return FIntVector(
            FMath::RoundToInt(v.X / precision),
            FMath::RoundToInt(v.Y / precision),
            FMath::RoundToInt(v.Z / precision)
        );
    };

	// Add or retrieve vertex index, merging duplicate vertices and accumulating normals
    auto GetOrAddVertex = [&](const FVector& v, const FVector& n, const FColor& c)
    {
        FIntVector key = Quantize(v);
        if (int32* existingIndex = vertexLookup.Find(key))
        {
			// Vertex already exists - accumulate its normal for better lighting
            finalNormals[*existingIndex] += n;
            return *existingIndex;
        }

		// Create new vertex entry
        int32 newIndex = finalVertices.Num();
        vertexLookup.Add(key, newIndex);
        finalVertices.Add(v);
        finalNormals.Add(n);
        finalColors.Add(c);
        return newIndex;
    };

	// Process all triangles and deduplicate vertices
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

		// Only add triangle if all three vertices are unique (skip degenerate triangles)
		if (i1 != i2 && i2 != i3 && i3 != i1)
		{
			finalTriangles.Append({i1, i2, i3});
		}
	}

	// Normalize all accumulated normals
    for (FVector& N : finalNormals)
    {
        N.Normalize();
    }

	// Create mesh section with final deduplicated data
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

// Get voxel density value with modification deltas applied
float AMarchingCubeGen::GetVoxelDensityWithModif(int X, int Y, int Z) const
{
	// Create voxel position key
	FIntVector Key(X, Y, Z);
	
	// Get base density from noise
	float BaseDensity = Voxels[GetVoxelIndex(X, Y, Z)];
	
	// Add any modifications made by the player (terrain destruction/building)
	if (const float* Delta = modifications.Find(Key))
	{
		BaseDensity += *Delta;
	}

	return BaseDensity;
}


// Modify voxels within a radius sphere for terrain destruction/creation
void AMarchingCubeGen::ModifyVoxel(const FVector& worldPos, float editingSpeed, float brushRadius)
{
    // Convert world position to local chunk coordinates
    FVector Local = (worldPos - GetActorLocation()) / 100.0f;

    // Calculate bounding box of voxels to modify
    int minX = FMath::FloorToInt(Local.X - brushRadius);
    int maxX = FMath::CeilToInt(Local.X + brushRadius);
    int minY = FMath::FloorToInt(Local.Y - brushRadius);
    int maxY = FMath::CeilToInt(Local.Y + brushRadius);
    int minZ = FMath::FloorToInt(Local.Z - brushRadius);
    int maxZ = FMath::CeilToInt(Local.Z + brushRadius);

    // Iterate through all voxels in the bounding box
    for (int x = minX; x <= maxX; x++)
    {
        for (int y = minY; y <= maxY; y++)
        {
            for (int z = minZ; z <= maxZ; z++)
            {
                // Calculate voxel center position
                FVector voxelCenter(x + 0.5f, y + 0.5f, z + 0.5f);
                float dist = FVector::Dist(voxelCenter, Local);

                // Only modify voxels within the brush radius
                if (dist < brushRadius)
                {
                    // Apply falloff effect (smooth gradient from center to edge)
                    float falloff = 1.0f - (dist / brushRadius);

                    // Calculate density change with time scaling for frame-rate independence
                    float deltaDensity = falloff * editingSpeed * GetWorld()->DeltaTimeSeconds;

                    // Apply modification to voxel, creating or destroying terrain
                    FIntVector voxelIndex(x, y, z);
                    float& currentDensity = modifications.FindOrAdd(voxelIndex, 0.0f);

                    // Accumulate the density change
                    currentDensity += deltaDensity;
                }
            }
        }
    }

	// Save modifications to disk
	SaveModifications();
    
    // Asynchronously rebuild the mesh on a thread pool
    TFuture<FThreadMeshData> Future = Async(EAsyncExecution::ThreadPool, [this]() -> FThreadMeshData
    {
        FThreadMeshData ThreadData;
        ThreadData.Reset();
        GenerateMesh(0, size, ThreadData);
        return ThreadData;
    });

    // Apply the updated mesh on the game thread
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

// Save voxel modifications to disk for persistence between sessions
void AMarchingCubeGen::SaveModifications()
{
	// Skip if no modifications to save
	if (modifications.Num() == 0)
		return;

	// Create a copy of modifications for async save
	TMap<FIntVector, float> ModCopy = modifications;

	// Create save directory path
	FString SaveDir = FPaths::ProjectSavedDir() / TEXT("VoxelChunks");
	IFileManager::Get().MakeDirectory(*SaveDir, true);

	// Calculate chunk coordinates from actor location
	FIntVector ChunkCoord(
		FMath::FloorToInt(GetActorLocation().X / (size * 100)),
		FMath::FloorToInt(GetActorLocation().Y / (size * 100)),
		FMath::FloorToInt(GetActorLocation().Z / (size * 100))
	);

	// Create unique filename based on chunk coordinates
	FString FileName = FString::Printf(TEXT("%s/Chunk_%d_%d_%d.sav"),
		*SaveDir, ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);

	// Save modifications asynchronously to avoid blocking the game thread
	Async(EAsyncExecution::ThreadPool, [FileName, ModCopy = MoveTemp(ModCopy)]()
	{
		// Format modifications as CSV data
		FString SaveData;
		SaveData.Reserve(ModCopy.Num() * 32); 

		// Convert each modification to a CSV line
		for (const auto& Pair : ModCopy)
		{
			SaveData += FString::Printf(TEXT("%d,%d,%d,%f\n"),
				Pair.Key.X, Pair.Key.Y, Pair.Key.Z, Pair.Value);
		}

		// Write data to file
		FFileHelper::SaveStringToFile(SaveData, *FileName);
	});
}

// Load voxel modifications from disk to restore terrain changes
void AMarchingCubeGen::LoadModifications()
{
	// Get the saved chunks directory
	FString SaveDir = FPaths::ProjectSavedDir() / TEXT("VoxelChunks");

	// Calculate chunk coordinates from actor location
	FIntVector ChunkCoord(
		FMath::FloorToInt(GetActorLocation().X / (size * 100)),
		FMath::FloorToInt(GetActorLocation().Y / (size * 100)),
		FMath::FloorToInt(GetActorLocation().Z / (size * 100))
	);

	// Create the filename for this chunk's save file
	FString FileName = FString::Printf(TEXT("%s/Chunk_%d_%d_%d.sav"),
		*SaveDir, ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);

	// If save file doesn't exist, no modifications to load
	if (!FPaths::FileExists(FileName))
		return;

	// Load the file contents
	FString LoadedData;
	FFileHelper::LoadFileToString(LoadedData, *FileName);

	// Parse the data line by line
	TArray<FString> Lines;
	LoadedData.ParseIntoArrayLines(Lines);

	// Process each modification line
	for (FString& Line : Lines)
	{
		// Parse CSV format: X,Y,Z,Density
		TArray<FString> Parts;
		Line.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 4) continue;

		// Extract voxel position and density value
		FIntVector Pos(
			FCString::Atoi(*Parts[0]),
			FCString::Atoi(*Parts[1]),
			FCString::Atoi(*Parts[2])
		);
		float Density = FCString::Atof(*Parts[3]);
		
		// Add modification to the map
		modifications.Add(Pos, Density);
	}
}