// Fill out your copyright notice in the Description page of Project Settings.


#include "PlayerCharacter.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "TerrainDestruct/Generation/MarchingCubeGen.h"


// Sets default values
APlayerCharacter::APlayerCharacter()
{
	// Set this pawn to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	// Create root scene component
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	// Create camera component
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->bUsePawnControlRotation = true; // Changed to true!

	// Create movement component
	MovementComponent = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("MovementComponent"));
	MovementComponent->MaxSpeed = MovementSpeed;
	MovementComponent->Acceleration = 4000.0f;
	MovementComponent->Deceleration = 8000.0f;

	// Enable controller rotation
	bUseControllerRotationPitch = true;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	CurrentMovementInput = FVector::ZeroVector;
	VerticalInput = 0.0f;
}

// Called when the game starts or when spawned
void APlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		// Set input mode to Game Only to capture mouse
		PlayerController->SetInputMode(FInputModeGameOnly());
		PlayerController->bShowMouseCursor = false;

		if (ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer())
		{
			if (UEnhancedInputLocalPlayerSubsystem* Subsystem = 
				ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer))
			{
				if (InputMappingContext)
				{
					Subsystem->AddMappingContext(InputMappingContext, 0);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("InputMappingContext is not set!"));
				}
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("Enhanced Input Subsystem not found! Is the plugin enabled?"));
			}
		}
	}
	
}

void APlayerCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D MovementVector = Value.Get<FVector2D>();
	CurrentMovementInput = FVector(MovementVector.Y, MovementVector.X, 0.0f);
}

void APlayerCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// Add yaw and pitch input
		AddControllerYawInput(LookAxisVector.X * LookSensitivity);
		AddControllerPitchInput(-LookAxisVector.Y * LookSensitivity);
	}
}

void APlayerCharacter::MoveUp(const FInputActionValue& Value)
{
	if (Value.GetValueType() == EInputActionValueType::Axis1D)
	{
		VerticalInput = Value.Get<float>();
	}
	// If using Boolean
	else if (Value.GetValueType() == EInputActionValueType::Boolean)
	{
		VerticalInput = Value.Get<bool>() ? 1.0f : 0.0f;
	}
}

void APlayerCharacter::MoveDown(const FInputActionValue& Value)
{
	// If using Axis1D
	if (Value.GetValueType() == EInputActionValueType::Axis1D)
	{
		VerticalInput = -Value.Get<float>();
	}
	// If using Boolean
	else if (Value.GetValueType() == EInputActionValueType::Boolean)
	{
		VerticalInput = Value.Get<bool>() ? -1.0f : 0.0f;
	}
}

void APlayerCharacter::DestroyTerrain(const FInputActionValue& Value)
{
	APlayerController* player = Cast<APlayerController>(GetController());
	if (!player) return;

	FVector Location;
	FRotator Rotation;
	player->GetPlayerViewPoint(Location, Rotation);

	FVector Start = Location;
	FVector End = Start + (Rotation.Vector() * DestroyRange);

	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		;
		if (AMarchingCubeGen* chunk = Cast<AMarchingCubeGen>(Hit.GetActor()))
		{
			currentModifyTimer += GetWorld()->GetTimeSeconds();
			if (currentModifyTimer > 0.2)
			{
				currentModifyTimer = 0.0f;
				chunk->ModifyVoxel(Hit.Location, -1.0f,4.0f);
			}

		}
	}
}

// Called every frame
void APlayerCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!CurrentMovementInput.IsZero() || VerticalInput != 0.0f)
	{
		// Get forward and right vectors based on camera rotation (only yaw)
		FRotator YawRotation(0, GetControlRotation().Yaw, 0);
		FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// Calculate movement direction
		FVector MovementDirection = 
			(ForwardDirection * CurrentMovementInput.X) + 
			(RightDirection * CurrentMovementInput.Y) +
			(FVector::UpVector * VerticalInput);

		// Add movement input
		AddMovementInput(MovementDirection, 1.0f);
	}
}


void APlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{

		if (MoveAction)
		{
			EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &APlayerCharacter::Move);
			EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Completed, this, &APlayerCharacter::Move);
		}


		if (LookAction)
		{
			EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &APlayerCharacter::Look);
		}


		if (UpAction)
		{
			EnhancedInputComponent->BindAction(UpAction, ETriggerEvent::Triggered, this, &APlayerCharacter::MoveUp);
			EnhancedInputComponent->BindAction(UpAction, ETriggerEvent::Completed, this, &APlayerCharacter::MoveUp);
		}


		if (DownAction)
		{
			EnhancedInputComponent->BindAction(DownAction, ETriggerEvent::Triggered, this, &APlayerCharacter::MoveDown);
			EnhancedInputComponent->BindAction(DownAction, ETriggerEvent::Completed, this, &APlayerCharacter::MoveDown);
		}


		if (ActionPressed)
		{
			EnhancedInputComponent->BindAction(ActionPressed, ETriggerEvent::Triggered, this, &APlayerCharacter::DestroyTerrain);
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Enhanced Input Component not found! Using legacy input system?"));
	}
}