#include "PlayerCharacter.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "TerrainDestruct/Generation/MarchingCubeGen.h"



// Constructor - initializes player character components and input settings
APlayerCharacter::APlayerCharacter()
{
	// Enable tick for per-frame updates
	PrimaryActorTick.bCanEverTick = true;

	// Create root scene component as the base for attachments
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	// Create and attach camera component
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	// Camera follows controller rotation for first-person view
	Camera->bUsePawnControlRotation = true;

	// Create floating movement component for free flight movement
	MovementComponent = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("MovementComponent"));
	MovementComponent->MaxSpeed = MovementSpeed;
	MovementComponent->Acceleration = 4000.0f;
	MovementComponent->Deceleration = 8000.0f;

	// Enable controller-based rotation for pitch and yaw input
	bUseControllerRotationPitch = true;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	// Initialize movement input variables
	CurrentMovementInput = FVector::ZeroVector;
	VerticalInput = 0.0f;
}


// Called when the game starts - initializes enhanced input system
void APlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Get the player controller and configure input settings
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		// Set input mode to game only (no UI interaction)
		PlayerController->SetInputMode(FInputModeGameOnly());
		// Hide the mouse cursor for immersive gameplay
		PlayerController->bShowMouseCursor = false;

		// Access the local player to set up enhanced input mapping
		if (ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer())
		{
			// Get the enhanced input subsystem from the local player
			if (UEnhancedInputLocalPlayerSubsystem* Subsystem = 
				ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer))
			{
				// Add the input mapping context if it's configured
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

// Process horizontal movement input (forward/backward and left/right)
void APlayerCharacter::Move(const FInputActionValue& Value)
{
	// Extract the 2D movement vector from the input action
	const FVector2D MovementVector = Value.Get<FVector2D>();
	// Convert 2D input to 3D movement vector (Y->X and X->Y for proper rotation handling)
	CurrentMovementInput = FVector(MovementVector.Y, MovementVector.X, 0.0f);
}

// Process camera look input (mouse or controller stick movement)
void APlayerCharacter::Look(const FInputActionValue& Value)
{
	// Extract the 2D look vector from the input action
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	// Apply camera rotation only if controller is valid
	if (Controller != nullptr)
	{
		// Apply yaw rotation (left/right movement)
		AddControllerYawInput(LookAxisVector.X * LookSensitivity);
		// Apply pitch rotation (up/down movement, inverted for intuitive controls)
		AddControllerPitchInput(-LookAxisVector.Y * LookSensitivity);
	}
}

// Process vertical movement input for ascending (Space or gamepad button)
void APlayerCharacter::MoveUp(const FInputActionValue& Value)
{
	// Handle analog stick input (value between 0 and 1)
	if (Value.GetValueType() == EInputActionValueType::Axis1D)
	{
		VerticalInput = Value.Get<float>();
	}
	// Handle binary button input (pressed or released)
	else if (Value.GetValueType() == EInputActionValueType::Boolean)
	{
		VerticalInput = Value.Get<bool>() ? 1.0f : 0.0f;
	}
}

// Process vertical movement input for descending (Ctrl or gamepad button)
void APlayerCharacter::MoveDown(const FInputActionValue& Value)
{
	// Handle analog stick input (negative value for downward movement)
	if (Value.GetValueType() == EInputActionValueType::Axis1D)
	{
		VerticalInput = -Value.Get<float>();
	}
	// Handle binary button input (pressed or released)
	else if (Value.GetValueType() == EInputActionValueType::Boolean)
	{
		VerticalInput = Value.Get<bool>() ? -1.0f : 0.0f;
	}
}

// Handle terrain destruction/modification when player shoots raycasts at terrain
void APlayerCharacter::DestroyTerrain(const FInputActionValue& Value)
{
	// Get the player controller to perform raycasting from camera
	APlayerController* player = Cast<APlayerController>(GetController());
	if (!player) return;

	// Get camera position and direction
	FVector Location;
	FRotator Rotation;
	player->GetPlayerViewPoint(Location, Rotation);

	// Define raycast start and end points
	FVector Start = Location;
	FVector End = Start + (Rotation.Vector() * DestroyRange);

	// Perform raycast to detect terrain chunk collision
	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);  // Don't hit the player

	// Cast a line trace and check if we hit a terrain chunk
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		// Check if the hit actor is a marching cubes terrain chunk
		if (AMarchingCubeGen* chunk = Cast<AMarchingCubeGen>(Hit.GetActor()))
		{
			// Accumulate time to throttle terrain modifications for performance
			currentModifyTimer += GetWorld()->GetTimeSeconds();
			// Only modify terrain every 0.2 seconds to avoid excessive mesh regeneration
			if (currentModifyTimer > 0.2)
			{
				currentModifyTimer = 0.0f;
				// Apply terrain destruction at hit location with radius of 4.0 units
				chunk->ModifyVoxel(Hit.Location, -1.0f, 4.0f);
			}

		}
	}
}


// Called every frame to process and apply accumulated movement input
void APlayerCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	
	// Only process movement if there's input to apply
	if (!CurrentMovementInput.IsZero() || VerticalInput != 0.0f)
	{
		// Create rotation matrix from controller yaw to get directional vectors
		FRotator YawRotation(0, GetControlRotation().Yaw, 0);
		FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// Combine horizontal and vertical movement inputs into final movement vector
		FVector MovementDirection = 
			(ForwardDirection * CurrentMovementInput.X) + 
			(RightDirection * CurrentMovementInput.Y) +
			(FVector::UpVector * VerticalInput);

		// Apply movement direction to the floating pawn movement component
		AddMovementInput(MovementDirection, 1.0f);
	}
}


// Set up all player input bindings for movement, camera, and actions
void APlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	
	// Cast to enhanced input component to use new input system
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Bind horizontal movement (WASD keys)
		if (MoveAction)
		{
			// Bind both triggered and completed events to capture button press and release
			EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &APlayerCharacter::Move);
			EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Completed, this, &APlayerCharacter::Move);
		}

		// Bind camera look input (Mouse movement or right stick)
		if (LookAction)
		{
			EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &APlayerCharacter::Look);
		}

		// Bind upward movement (Space bar or gamepad button)
		if (UpAction)
		{
			// Bind both triggered and completed events to capture button press and release
			EnhancedInputComponent->BindAction(UpAction, ETriggerEvent::Triggered, this, &APlayerCharacter::MoveUp);
			EnhancedInputComponent->BindAction(UpAction, ETriggerEvent::Completed, this, &APlayerCharacter::MoveUp);
		}

		// Bind downward movement (Ctrl key or gamepad button)
		if (DownAction)
		{
			// Bind both triggered and completed events to capture button press and release
			EnhancedInputComponent->BindAction(DownAction, ETriggerEvent::Triggered, this, &APlayerCharacter::MoveDown);
			EnhancedInputComponent->BindAction(DownAction, ETriggerEvent::Completed, this, &APlayerCharacter::MoveDown);
		}

		// Bind terrain destruction action (Left mouse button or gamepad trigger)
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