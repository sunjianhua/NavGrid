// Fill out your copyright notice in the Description page of Project Settings.

#include "NavGridPrivatePCH.h"

#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Animation/AnimInstance.h"

UGridMovementComponent::UGridMovementComponent(const FObjectInitializer &ObjectInitializer)
	:Super(ObjectInitializer)
{
	PathMesh = ConstructorHelpers::FObjectFinder<UStaticMesh>(TEXT("StaticMesh'/NavGrid/SMesh/NavGrid_Path.NavGrid_Path'")).Object;

	AvailableMovementModes.Add(EGridMovementMode::ClimbingDown);
	AvailableMovementModes.Add(EGridMovementMode::ClimbingUp);
	AvailableMovementModes.Add(EGridMovementMode::Stationary);
	AvailableMovementModes.Add(EGridMovementMode::Walking);
	AvailableMovementModes.Add(EGridMovementMode::InPlaceTurn);

	Distance = 0;
	MovementMode = EGridMovementMode::Stationary;
	MovementPhase = EGridMovementPhase::Done;
}

void UGridMovementComponent::BeginPlay()
{
	Super::BeginPlay();

	/* I dont know why, but if we use createdefaultsubobject in the constructor this is sometimes reset to NULL*/
	if (!IsValid(Spline))
	{
		Spline = NewObject<USplineComponent>(this, "PathSpline");
		check(Spline);
	}

	Grid = ANavGrid::GetNavGrid(GetWorld());
	if (!Grid)
	{
		UE_LOG(NavGrid, Error, TEXT("%s was unable to find a NavGrid in level"), *this->GetName());
	}

	/* Grab a reference to (a) AnimInstace */
	for (UActorComponent *Comp : GetOwner()->GetComponentsByClass(USkeletalMeshComponent::StaticClass()))
	{
		USkeletalMeshComponent *Mesh = Cast<USkeletalMeshComponent>(Comp);
		if (Mesh)
		{
			MeshRotation = Mesh->GetRelativeTransform().Rotator();
			AnimInstance = Mesh->GetAnimInstance();
			if (AnimInstance)
			{
				break;
			}
		}
	}
	if (!AnimInstance)
	{
		UE_LOG(NavGrid, Error, TEXT("%s: Unable to get reference to AnimInstance"), *GetName());
		bUseRootMotion = false;
		bAlwaysUseRootMotion = false;
	}
}

void UGridMovementComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AActor *Owner = GetOwner();
	FTransform NewTransform = Owner->GetActorTransform();
	FRotator ActorRotation = Owner->GetActorRotation();

	ConsiderUpdateMovementMode();
	switch (MovementMode)
	{
	default:
	case EGridMovementMode::Stationary:
		if (bAlwaysUseRootMotion && MovementPhase != EGridMovementPhase::Ending)
		{
			FTransform RootMotion = ConsumeRootMotion();
			NewTransform.SetRotation(NewTransform.GetRotation() * RootMotion.GetRotation());
			FRotator AnimToWorld = Owner->GetActorRotation() + MeshRotation;
			NewTransform.SetLocation(NewTransform.GetLocation() + AnimToWorld.RotateVector(RootMotion.GetLocation()));

		}
		break; //nothing to do
	case EGridMovementMode::InPlaceTurn:
		NewTransform = TransformFromRotation(DeltaTime);
		break;
	case EGridMovementMode::Walking:
	case EGridMovementMode::ClimbingDown:
	case EGridMovementMode::ClimbingUp:
		NewTransform = TransformFromPath(DeltaTime);
		break;
	}

	/* reset rotation if we have any rotation locks */
	FRotator NewRotation = NewTransform.GetRotation().Rotator();
	NewRotation.Roll = LockRoll ? ActorRotation.Roll : NewRotation.Roll;
	NewRotation.Pitch = LockPitch ? ActorRotation.Pitch: NewRotation.Pitch;
	NewRotation.Yaw = LockYaw ? ActorRotation.Yaw : NewRotation.Yaw;
	NewTransform.SetRotation(NewRotation.Quaternion());
	/* never change the scale */
	NewTransform.SetScale3D(Owner->GetActorScale3D());

	// update velocity so it can be fetched by the pawn
	Velocity = (NewTransform.GetLocation() - Owner->GetActorLocation()) * (1 / DeltaTime);
	UpdateComponentVelocity();
	// actually move the the actor
	Owner->SetActorTransform(NewTransform);

	if (MovementPhase == EGridMovementPhase::Beginning)
	{
		MovementPhase = EGridMovementPhase::Middle;
	}
	else if (MovementPhase == EGridMovementPhase::Ending && Velocity.Size() < 25)
	{
		OnMovementEndEvent.Broadcast();
		MovementPhase = EGridMovementPhase::Done;
	}
}

FTransform UGridMovementComponent::TransformFromPath(float DeltaTime)
{
	/* Check if we can get the speed from root motion */
	float CurrentSpeed = 0;
	if (bUseRootMotion)
	{
		FTransform RootMotion = ConsumeRootMotion();
		CurrentSpeed = RootMotion.GetLocation().Size();
	}

	if (CurrentSpeed < 25 * DeltaTime)
	{
		if (MovementMode == EGridMovementMode::ClimbingDown || MovementMode == EGridMovementMode::ClimbingUp)
		{
			CurrentSpeed = MaxClimbSpeed * DeltaTime;
		}
		else
		{
			CurrentSpeed = MaxWalkSpeed * DeltaTime;
		}
	}

	Distance = FMath::Min(Spline->GetSplineLength(), Distance + CurrentSpeed);

	/* Grab our current transform so we can find the velocity if we need it later */
	AActor *Owner = GetOwner();
	FTransform OldTransform = Owner->GetTransform();

	/* Find the next location and rotation from the spline*/
	FTransform NewTransform = Spline->GetTransformAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::Local);
	FRotator DesiredRotation;

	/* Restrain rotation axis if we're walking */
	if (MovementMode == EGridMovementMode::Walking)
	{
		DesiredRotation = NewTransform.Rotator();
	}
	/* Use the rotation from the ladder if we're climbing */
	else if (MovementMode == EGridMovementMode::ClimbingUp || MovementMode == EGridMovementMode::ClimbingDown)
	{
		if (Grid)
		{
			UNavTileComponent *CurrentTile = Grid->GetTile(PawnOwner->GetActorLocation(), false);
			if (CurrentTile)
			{
				DesiredRotation = CurrentTile->GetComponentRotation();
				DesiredRotation.Yaw -= 180;
			}
		}
		else
			// Dont update the rotation if we have no idea of what it should be
			// This to prevent the occational stuttering at the start of a descent
		{
			DesiredRotation = GetOwner()->GetActorRotation();
		}
	}

	/* Find the new rotation by limiting DesiredRotation by MaxRotationSpeed */
	FRotator NewRotation = LimitRotation(OldTransform.GetRotation().Rotator(), DesiredRotation, DeltaTime);
	NewTransform.SetRotation(NewRotation.Quaternion());

	/* Check if we are in the end phase of this movement mode */
	if (Distance + StoppingDistance >= Spline->GetSplineLength())
	{
		MovementPhase = EGridMovementPhase::Ending;
	}
	/* clear path if we have traversed it all (we might not get here if StoppingDistance is set) */
	if (CurrentSpeed == 0 || Distance >= Spline->GetSplineLength())
	{
		ChangeMovementMode(EGridMovementMode::Stationary);
		MovementPhase = EGridMovementPhase::Ending;
		Distance = 0;
		Spline->ClearSplinePoints();
	}

	return NewTransform;
}

FTransform UGridMovementComponent::TransformFromRotation(float DeltaTime)
{
	AActor *Owner = GetOwner();
	FTransform NewTransform = Owner->GetActorTransform();
	if (Owner->GetActorRotation().Equals(DesiredForwardRotation))
	{
		ChangeMovementMode(EGridMovementMode::Stationary);
	}
	else
	{
		FRotator NewRotation = LimitRotation(Owner->GetActorRotation(), DesiredForwardRotation, DeltaTime);
		NewTransform.SetRotation(NewRotation.Quaternion());
	}
	return NewTransform;
}

void UGridMovementComponent::GetTilesInRange(TArray<UNavTileComponent *> &OutTiles)
{
	check(Grid);
	Grid->GetTilesInRange(Cast<AGridPawn>(GetOwner()), true, OutTiles);
}

UNavTileComponent *UGridMovementComponent::GetTile()
{
	return GetTile(GetOwner()->GetActorLocation());
}

UNavTileComponent * UGridMovementComponent::GetTile(const FVector &Position)
{
	UNavTileComponent *CurrentTile = Grid->GetTile(Position, true);
	if (!CurrentTile &&
		(AvailableMovementModes.Contains(EGridMovementMode::ClimbingDown) ||
		AvailableMovementModes.Contains(EGridMovementMode::ClimbingUp)))
	{
		CurrentTile = Grid->GetTile(Position, false);
	}
	return CurrentTile;
}

void UGridMovementComponent::StringPull(TArray<const UNavTileComponent*>& InPath, TArray<const UNavTileComponent*>& OutPath)
{
	AGridPawn *GridPawnOwner = Cast<AGridPawn>(GetOwner());

	OutPath.Empty();
	if (GridPawnOwner && InPath.Num() > 2)
	{
		const UCapsuleComponent &Capsule = *GridPawnOwner->CapsuleComponent;
		int32 CurrentIdx = 0;
		OutPath.Add(InPath[0]);
		for (int32 Idx = 1; Idx < InPath.Num(); Idx++)
		{
			if (FMath::Abs(InPath[CurrentIdx]->GetPawnLocation().Z - InPath[Idx]->GetPawnLocation().Z) > 30 ||
				InPath[Idx]->Obstructed(InPath[CurrentIdx]->GetPawnLocation(), Capsule))
			{
				OutPath.AddUnique(InPath[Idx - 1]);
				CurrentIdx = Idx - 1;
			}
			// dont stringpull ladders
			else if (Cast<const UNavLadderComponent>(InPath[Idx]))
			{
				OutPath.AddUnique(InPath[Idx - 1]);
				OutPath.AddUnique(InPath[Idx]);
				if (Idx + 1 < InPath.Num())
				{
					OutPath.AddUnique(InPath[Idx + 1]);
				}
				CurrentIdx = Idx + 1;
				Idx = Idx + 1;
			}
		}
		OutPath.Add(InPath[InPath.Num() - 1]);
	}
	else
	{
		OutPath = InPath;
	}
}

bool UGridMovementComponent::CreatePath(const UNavTileComponent &Target)
{
	Spline->ClearSplinePoints();
	AGridPawn *Owner = Cast<AGridPawn>(GetOwner());

	UNavTileComponent *Tile = NULL;
	if (Grid)
	{
		Tile = Grid->GetTile(Owner->GetActorLocation());
	}
	if (!Tile)
	{
		UE_LOG(NavGrid, Error, TEXT("%s: Not on grid"), *Owner->GetName());
		return false;
	}

	TArray<UNavTileComponent *> InRange;
	Grid->GetTilesInRange(Cast<AGridPawn>(GetOwner()), true, InRange);
	if (InRange.Contains(&Target))
	{
		// create a list of tiles from the destination to the starting point and reverse it
		TArray<const UNavTileComponent *> Path;
		const UNavTileComponent *Current = &Target;
		while (Current)
		{
			Path.Add(Current);
			Current = Current->Backpointer;
		}
		Algo::Reverse(Path);

		if (bStringPullPath)
		{
			TArray<const UNavTileComponent *> StringPulledPath;
			StringPull(Path, StringPulledPath);
			Path = StringPulledPath;
		}

		// first add a spline point for the starting location
		Spline->AddSplinePoint(GetOwner()->GetActorLocation(), ESplineCoordinateSpace::Local);

		// Add spline points for the tiles in the path
		for (int32 Idx = 1; Idx < Path.Num(); Idx++)
		{
			Path[Idx]->AddSplinePoints(Path[Idx - 1]->GetComponentLocation(), *Spline, Idx == Path.Num() - 1);
		}

		if (Spline->GetSplineLength() == 0)
		{
			UE_LOG(NavGrid, Error, TEXT("UGridMovementComponent::CreatePath() Ended up with a zero-length spline"));
			return false;
		}

		return true; // success!
	}
	else
	{
		return false; // no path to Target
	}
}

bool UGridMovementComponent::MoveTo(const UNavTileComponent &Target)
{
	bool PathExists = CreatePath(Target);
	if (PathExists)
	{
		ChangeMovementMode(EGridMovementMode::Walking);
	}
	return PathExists;
}

void UGridMovementComponent::TurnTo(const FRotator & Forward)
{
	if (AvailableMovementModes.Contains(EGridMovementMode::InPlaceTurn))
	{
		DesiredForwardRotation = Forward;
		ChangeMovementMode(EGridMovementMode::InPlaceTurn);
	}
}

void UGridMovementComponent::SnapToGrid()
{
	AGridPawn *GridPawnOwner = Cast<AGridPawn>(GetOwner());
	check(GridPawnOwner);
	/* generate virtual tiles to increase tha chance of the pawn beeing on a tile*/
	if (Grid->EnableVirtualTiles)
	{
		Grid->GenerateVirtualTiles(GridPawnOwner);
	}

	UNavTileComponent *SnapTile = GetTile();
	if (SnapTile)
	{
		GridPawnOwner->SetActorLocation(SnapTile->GetPawnLocation());
	}
}

void UGridMovementComponent::ShowPath()
{
	if (PathMesh)
	{
		float PathDistance = HorizontalOffset; // Get some distance between the actor and the path
		FBoxSphereBounds Bounds = PathMesh->GetBounds();
		float MeshLength = FMath::Abs(Bounds.BoxExtent.X) * 2;
		float SplineLength = Spline->GetSplineLength();
		SplineLength -= HorizontalOffset; // Get some distance between the cursor and the path

		while (PathDistance < SplineLength)
		{
			AddSplineMesh(PathDistance, FMath::Min(PathDistance + MeshLength, SplineLength));
			PathDistance += FMath::Min(MeshLength, SplineLength - PathDistance);
		}
	}
}

void UGridMovementComponent::HidePath()
{
	for (USplineMeshComponent *SMesh : SplineMeshes)
	{
		SMesh->DestroyComponent();
	}
	SplineMeshes.Empty();
}

FTransform UGridMovementComponent::ConsumeRootMotion()
{
	if (!AnimInstance)
	{
		return FTransform();
	}

	FRootMotionMovementParams RootMotionParams = AnimInstance->ConsumeExtractedRootMotion(1);
	return RootMotionParams.GetRootMotionTransform();
}


void UGridMovementComponent::ConsiderUpdateMovementMode()
{
	if (MovementMode == EGridMovementMode::Walking ||
		MovementMode == EGridMovementMode::ClimbingDown ||
		MovementMode == EGridMovementMode::ClimbingUp)
	{
		// try to get the tile the pawn will occupy when it moves a bit further
		FVector ForwardPoint = GetForwardLocation(50);
		UNavTileComponent *CurrentTile = GetTile(ForwardPoint);
		if (CurrentTile)
		{
			if (CurrentTile->IsA<UNavLadderComponent>())
			{
				if (ForwardPoint.Z > GetActorLocation().Z)
				{
					ChangeMovementMode(EGridMovementMode::ClimbingUp);
				}
				else
				{
					ChangeMovementMode(EGridMovementMode::ClimbingDown);
				}
			}
			else
			{
				ChangeMovementMode(EGridMovementMode::Walking);
			}
		}
	}
}

void UGridMovementComponent::ChangeMovementMode(EGridMovementMode NewMode)
{
	if (NewMode != MovementMode)
	{
		OnMovementModeChangedEvent.Broadcast(MovementMode, NewMode);
		MovementMode = NewMode;
		if (MovementMode != EGridMovementMode::Stationary)
		{
			MovementPhase = EGridMovementPhase::Beginning;
		}
	}
}

FVector UGridMovementComponent::GetForwardLocation(float ForwardDistance)
{
	float D = FMath::Min(Spline->GetSplineLength(), Distance + ForwardDistance);
	return Spline->GetLocationAtDistanceAlongSpline(D, ESplineCoordinateSpace::Local);
}

void UGridMovementComponent::AddSplineMesh(float From, float To)
{
	float TanScale = 25;
	FVector StartPos = Spline->GetLocationAtDistanceAlongSpline(From, ESplineCoordinateSpace::Local);
	StartPos.Z += Grid->UIOffset;
	FVector StartTan = Spline->GetDirectionAtDistanceAlongSpline(From, ESplineCoordinateSpace::Local) * TanScale;
	FVector EndPos = Spline->GetLocationAtDistanceAlongSpline(To, ESplineCoordinateSpace::Local);
	EndPos.Z += Grid->UIOffset;
	FVector EndTan = Spline->GetDirectionAtDistanceAlongSpline(To, ESplineCoordinateSpace::Local) * TanScale;
	FVector UpVector = EndPos - StartPos;
	UpVector = FVector(UpVector.Y, UpVector.Z, UpVector.X);

	UPROPERTY() USplineMeshComponent *SplineMesh = NewObject<USplineMeshComponent>(this);
	SplineMesh->SetMobility(EComponentMobility::Movable);
	SplineMesh->SetStartAndEnd(StartPos, StartTan, EndPos, EndTan);
	SplineMesh->SetStaticMesh(PathMesh);
	SplineMesh->RegisterComponentWithWorld(GetWorld());
	SplineMesh->SetSplineUpDir(UpVector);
	SplineMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SplineMeshes.Add(SplineMesh);
}

FRotator UGridMovementComponent::LimitRotation(const FRotator &OldRotation, const FRotator &NewRotation, float DeltaTime)
{
	FRotator Result = OldRotation.GetNormalized();
	FRotator DeltaRotation = NewRotation - OldRotation;
	DeltaRotation.Normalize();
	Result.Pitch += DeltaRotation.Pitch > 0 ? FMath::Min<float>(DeltaRotation.Pitch, MaxRotationSpeed * DeltaTime) :
		FMath::Max<float>(DeltaRotation.Pitch, MaxRotationSpeed * -DeltaTime);
	Result.Roll += DeltaRotation.Roll > 0 ? FMath::Min<float>(DeltaRotation.Roll, MaxRotationSpeed * DeltaTime) :
		FMath::Max<float>(DeltaRotation.Roll, MaxRotationSpeed * -DeltaTime);
	Result.Yaw += DeltaRotation.Yaw > 0 ? FMath::Min<float>(DeltaRotation.Yaw, MaxRotationSpeed * DeltaTime) :
		FMath::Max<float>(DeltaRotation.Yaw, MaxRotationSpeed * -DeltaTime);

	return Result;
}
