#include "GenerateTerrain.h"
#include "MarchingCubeGen.h"
#include "Kismet/GameplayStatics.h"

// Constructor for the terrain generator
AGenerateTerrain::AGenerateTerrain()
{
	// Enable the actor to be updated every frame
	PrimaryActorTick.bCanEverTick = true;
}


// Called when the game starts
void AGenerateTerrain::BeginPlay()
{
	Super::BeginPlay();
	// Generate the initial world
	GenerateWorld();

}


// Called every frame to update the game
void AGenerateTerrain::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Load pending chunks progressively (ChunkLoadPerFrame chunks per frame)
	for (int i = 0; i < ChunkLoadPerFrame && !PendingChunks.IsEmpty(); ++i)
	{
		FIntVector chunkCoords;
		PendingChunks.Dequeue(chunkCoords);
		SpawnChunkAt(chunkCoords);
	}

	// Get the current player position
	FVector PlayerPos = GetWorld()->GetFirstPlayerController()->GetPawn()->GetActorLocation();
	
	// Unload chunks that are too far from the player
	for (auto it = LoadedChunks.CreateIterator(); it; ++it)
	{
		if (it.Value() && FVector::Dist(it.Value()->GetActorLocation(), PlayerPos) > drawDistance * size * 100)
		{
			it.Value()->Destroy();
			it.RemoveCurrent();
		}
	}

	// Continuously generate chunks around the player
	GenerateWorld();
}



// Generate all chunks around the player within a radius of drawDistance
void AGenerateTerrain::GenerateWorld()
{
	// Get the coordinates of the chunk in which the player is located
	FIntVector PlayerChunk = GetPlayerChunk();

	// Iterate through all chunks within the drawDistance radius
	for (int x = -drawDistance; x <= drawDistance; x++)
	{
		for (int y = -drawDistance; y <= drawDistance; y++)
		{
			for (int z = -drawDistance; z <= drawDistance; z++)
			{
				// Calculate the coordinates of the chunk to be generated
				FIntVector chunkCoords = PlayerChunk + FIntVector(x, y, z);
				
				// If the chunk has not been generated yet, add it to the queue
				if (!LoadedChunks.Contains(chunkCoords))
				{
					PendingChunks.Enqueue(chunkCoords);
					LoadedChunks.Add(chunkCoords, nullptr); 
				}
			}
		}
	}
}

// Returns the coordinates of the chunk in which the player is located
FIntVector AGenerateTerrain::GetPlayerChunk() const
{
	// Get the current player position
	FVector PlayerPos = GetWorld()->GetFirstPlayerController()->GetPawn()->GetActorLocation();
	
	// Divide the position by the chunk size to get the chunk coordinates
	int32 cx = FMath::FloorToInt(PlayerPos.X / (size * 100));
	int32 cy = FMath::FloorToInt(PlayerPos.Y / (size * 100));
	int32 cz = FMath::FloorToInt(PlayerPos.Z / (size * 100));
	
	return FIntVector(cx, cy, cz);
}

// Creates and initializes a chunk at the specified coordinates
void AGenerateTerrain::SpawnChunkAt(const FIntVector& chunkCoords)
{
	// Calculate the world position based on chunk coordinates
	FVector WorldPos = FVector(chunkCoords.X * size * 100, chunkCoords.Y * size * 100, chunkCoords.Z * size * 100);
	FTransform transform(FRotator::ZeroRotator, WorldPos, FVector::OneVector);

	// Create the chunk deferred to avoid BeginPlay being called before initialization
	AMarchingCubeGen* chunk = GetWorld()->SpawnActorDeferred<AMarchingCubeGen>(
		AMarchingCubeGen::StaticClass(),
		transform,
		this
	);

	// Initialize chunk parameters
	chunk->frequency = frequency;
	chunk->material = material;
	chunk->size = size;
	chunk->surfaceLevel = surfaceLevel;
	chunk->LoadModifications();
	
	// Finalize chunk creation and add it to the world
	UGameplayStatics::FinishSpawningActor(chunk, transform);
	LoadedChunks[chunkCoords] = chunk;
}