#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GenerateTerrain.generated.h"


class AMarchingCubeGen;
UCLASS()
class TERRAINDESTRUCT_API AGenerateTerrain : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AGenerateTerrain();

	UPROPERTY(EditInstanceOnly, Category="Generation")
	int drawDistance = 5;
	
	UPROPERTY(EditInstanceOnly, Category="Generation")
	float frequency = 0.03f;
	
	UPROPERTY(EditInstanceOnly, Category="Generation")
	int size = 32;

	UPROPERTY(EditInstanceOnly, Category="Generation")
	TObjectPtr<UMaterialInterface> material;


	TMap<FIntVector, AMarchingCubeGen*> LoadedChunks;

	UPROPERTY(EditAnywhere)
	int32 ChunkLoadPerFrame = 4;  // How many chunks to spawn per frame

	TQueue<FIntVector> PendingChunks; // Queue of chunks to generate
	
protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	FIntVector GetPlayerChunk() const;
	void SpawnChunkAt(const FIntVector& ChunkCoords);
	void GenerateWorld();
};