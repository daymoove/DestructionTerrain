#include "GenerateTerrain.h"
#include "MarchingCubeGen.h"
#include "Kismet/GameplayStatics.h"

AGenerateTerrain::AGenerateTerrain()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void AGenerateTerrain::BeginPlay()
{
	Super::BeginPlay();
	GenerateWorld();
	
}

// Called every frame
void AGenerateTerrain::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Spawn some chunks per frame to avoid blocking
	for (int i = 0; i < ChunkLoadPerFrame && !PendingChunks.IsEmpty(); ++i)
	{
		FIntVector chunkCoords;
		PendingChunks.Dequeue(chunkCoords);
		SpawnChunkAt(chunkCoords);
	}

	// Optionally unload distant chunks
	FVector PlayerPos = GetWorld()->GetFirstPlayerController()->GetPawn()->GetActorLocation();
	for (auto it = LoadedChunks.CreateIterator(); it; ++it)
	{
		if (it.Value() && FVector::Dist(it.Value()->GetActorLocation(), PlayerPos) > drawDistance * size * 100)
		{
			it.Value()->Destroy();
			it.RemoveCurrent();
		}
	}

	// Enqueue new chunks around player
	GenerateWorld();
}



void AGenerateTerrain::GenerateWorld()
{
	FIntVector PlayerChunk = GetPlayerChunk();

	for (int x = -drawDistance; x <= drawDistance; x++)
	{
		for (int y = -drawDistance; y <= drawDistance; y++)
		{
			for (int z = -drawDistance; z <= drawDistance; z++)
			{
				FIntVector chunkCoords = PlayerChunk + FIntVector(x, y, z);
				if (!LoadedChunks.Contains(chunkCoords))
				{
					PendingChunks.Enqueue(chunkCoords);
					LoadedChunks.Add(chunkCoords, nullptr); // reserve spot
				}
			}
		}
	}
}


FIntVector AGenerateTerrain::GetPlayerChunk() const
{
	FVector PlayerPos = GetWorld()->GetFirstPlayerController()->GetPawn()->GetActorLocation();
	int32 cx = FMath::FloorToInt(PlayerPos.X / (size * 100));
	int32 cy = FMath::FloorToInt(PlayerPos.Y / (size * 100));
	int32 cz = FMath::FloorToInt(PlayerPos.Z / (size * 100));
	return FIntVector(cx, cy, cz);
}

void AGenerateTerrain::SpawnChunkAt(const FIntVector& chunkCoords)
{
	FVector WorldPos = FVector(chunkCoords.X * size * 100, chunkCoords.Y * size * 100, chunkCoords.Z * size * 100);
	FTransform transform(FRotator::ZeroRotator, WorldPos, FVector::OneVector);

	AMarchingCubeGen* chunk = GetWorld()->SpawnActorDeferred<AMarchingCubeGen>(
		AMarchingCubeGen::StaticClass(),
		transform,
		this
	);

	chunk->frequency = frequency;
	chunk->material = material;
	chunk->size = size;
	chunk->LoadModifications();
	
	UGameplayStatics::FinishSpawningActor(chunk, transform);

	// Assign chunk to map
	LoadedChunks[chunkCoords] = chunk;
}
