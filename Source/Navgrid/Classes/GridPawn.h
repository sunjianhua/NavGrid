// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "GameFramework/Pawn.h"
#include "GenericTeamAgentInterface.h"

#include "GridMovementComponent.h"
#include "GridPawn.generated.h"

class UTurnComponent;
class UCapsuleComponent;
class UArrowComponent;
class UNavTileComponent;
class ANavGrid;

UENUM()
enum class EGridPawnState : uint8
{
	/* it is not this pawns turn */
	WaitingForTurn	UMETA(DisplayName = "Waiting for turn"),
	/* Ready for player input */
	Ready			UMETA(DisplayName = "Ready"),
	/* Currently performing some sort of action and is not ready for player input */
	Busy			UMETA(DisplayName = "Busy"),
	/* Dead */
	Dead			UMETA(DisplayName = "Dead")
};

/**
 * A pawn that can move around on a NavGrid.
 *
 * Currently simply a pawn with a GridMovementComponent.
 */
UCLASS()
class NAVGRID_API AGridPawn : public APawn, public IGenericTeamAgentInterface
{
	GENERATED_BODY()

public:
	// Sets default values for this pawn's properties
	AGridPawn();
	virtual void BeginPlay() override;

	// IGenericTeamAgentInterface start
	virtual void SetGenericTeamId(const FGenericTeamId& TeamID) override { this->TeamID = TeamID; }
	virtual FGenericTeamId GetGenericTeamId() const override { return TeamID; }
	// IGenericTeamAgentInterface end
protected:
	UPROPERTY(EditAnyWhere, BlueprintReadWrite, Category = "NavGrid")
	FGenericTeamId TeamID;

public:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	USceneComponent *Scene;
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Components")
	UGridMovementComponent *MovementComponent;
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Components")
	UTurnComponent *TurnComponent;
	/* Used to test if thre's room for pawn on a tile*/
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Components")
	UCapsuleComponent *CapsuleComponent;
	/* Shown when the pawn is selected/has its turn */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Components")
	UStaticMeshComponent *SelectedHighlight;
	/* An arrow pointing forward */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Components")
	UArrowComponent *Arrow;

	/* Should this pawn snap to grid at the start of each turn */
	UPROPERTY(EditAnyWhere, Category = "NavGrid")
	bool SnapToGrid = true;

	/* Callend on round start */
	virtual void OnRoundStart() {}
	/* Called on turn start */
	virtual void OnTurnStart();
	/* Called on turn end */
	virtual void OnTurnEnd();
	/* Called when done moving */
	virtual void OnMoveEnd();
	/* Called when this pawn is ready for player input */
	virtual void OnPawnReady() {};

	/* override this class and implement your own AI here. The default implementation just ends the turn */
	virtual void PlayAITurn();
	/* Get the current state for this pawn */
	UFUNCTION(BlueprintCallable, Category = "NavGrid")
	virtual EGridPawnState GetState() const;
	/* is this pawn controlled by a human player? */
	UPROPERTY(EditAnyWhere, BlueprintReadWrite, Category = "NavGrid")
	bool bHumanControlled;
	/* can the we request to start our turn now? The turn manager may still deny our request even if this returns true */
	virtual bool CanBeSelected();

	virtual bool CanMoveTo(const UNavTileComponent & Tile);
	virtual void MoveTo(const UNavTileComponent & Tile);
	/* get the tile occupied at the start of this pawns turn */
	UFUNCTION(BlueprintCallable, Category = "NavGrid")
	UNavTileComponent *GetTile() const { return MovementComponent->GetTile(); }
	template <class T>
	T *GetTile() const { return Cast<T>(GetTile()); }

	/* Called when the user clicks on this actor, default implementation is to change the the current turn taker to this */
	UFUNCTION()
	virtual void Clicked(AActor *ClickedActor, FKey PressedKey);

protected:
	UPROPERTY()
	ANavGrid *Grid;
};
