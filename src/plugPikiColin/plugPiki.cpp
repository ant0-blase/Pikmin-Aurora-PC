#include "App.h"

#include "AtxStream.h"
#include "DebugLog.h"
#include "Graphics.h"
#include "Interface.h"
#include "gameflow.h"
#include "sysMath.h"
#include "sysNew.h"
#include "timers.h"

#define TIMER_STATE_X           (32) ///< Horizontal position to start printing timer debug text from.
#define TIMER_STATE_Y           (32) ///< Vertical position to start printing timer debug text from.
#define TIMER_STATE_LINE_HEIGHT (12) ///< How far down to offset each line of timer debug text from the previous.

/**
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("plugPiki")

/**
 * @brief Performs a full system reset, including timers and the overlay heap, intended for boot setup.
 */
void PlugPikiApp::hardReset()
{
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] PlugPikiApp::hardReset begin\n");
#endif
	// use system heap for resetting game flow
	useHeap(SYSHEAP_Sys);

	gsys->mTimer = new Timers;
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] PlugPikiApp timer ready=%p\n", gsys->mTimer);
	OSReport("[pikmin::boot] entering GameFlow::hardReset\n");
#endif
	gameflow.hardReset(this);
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] GameFlow::hardReset complete\n");
#endif

	AyuHeap* sysHeap    = gsys->getHeap(SYSHEAP_Sys);
	int overlayHeapSize = sysHeap->getMaxFree();
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] preparing overlay heap maxFree=%d\n", overlayHeapSize);
#endif

	// allocate space for overlay heap from the system heap
	int oldAlloc = sysHeap->setAllocType(AYU_STACK_GROW_UP);
	u8* buf      = new u8[sysHeap->getMaxFree()];
	sysHeap->setAllocType(oldAlloc);
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] overlay backing buffer=%p size=%d\n", buf, overlayHeapSize);
#endif

	// set up overlay heap using all remaining free space from system heap
	gsys->getHeap(SYSHEAP_Ovl)->init("ovl", AYU_STACK_GROW_UP, buf, overlayHeapSize);
	gsys->resetHeap(SYSHEAP_Ovl, AYU_STACK_GROW_DOWN);
	gsys->getHeap(SYSHEAP_Ovl)->setAllocType(AYU_STACK_GROW_DOWN);
	useHeap(SYSHEAP_Ovl);
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] overlay heap initialized\n");
#endif

	// force a transition to a game section
	gsys->softReset();
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] PlugPikiApp::hardReset complete (soft reset queued)\n");
#endif
}

/**
 * @brief Performs a partial system reset, re-initialising sections and resources, and primes the app for drawing.
 */
void PlugPikiApp::softReset()
{
	BaseApp::softReset();
	gameflow.softReset();
	mIsReadyToDraw = TRUE;
}

/**
 * @brief Updates the game state by one frame, by iteratively updating its child nodes.
 *
 * `gameflow`'s `mFlowManager` is a child of this, and therefore gets updated by `Node::update`.
 * `mFlowManager` itself contains the current game section as a child node, which therefore gets updated,
 * causing everything else game-related to update.
 */
void PlugPikiApp::update()
{
	gameflow.mAppTickCounter++;
	gameflow.update();

	// iteratively update all our child nodes
	Node::update();
}

/**
 * @brief Draws the current frame, by iteratively drawing its child nodes. Also draws some debug text, if enabled.
 *
 * `gameflow`'s `mFlowManager` is a child of this, and therefore gets drawn by `Node::draw`.
 * `mFlowManager` itself contains the current game section as a child node, which therefore gets drawn,
 * causing everything else game-related to be rendered.
 *
 * @param gfx Graphics context for rendering.
 */
void PlugPikiApp::draw(Graphics& gfx)
{
	if (!mIsReadyToDraw) {
		return;
	}

	gsys->mTimer->start("cpu draw", true);
	gsys->mDispCount        = 0;
	gsys->mMaterialCount    = 0;
	gsys->mPolygonCount     = 0;
	gsys->mActiveLightCount = 0;
	gsys->mLightCount       = 0;
	gsys->mAnimatedPolygons = 0;
	gsys->mLightingSkips    = 0;
	gsys->mLightingSets     = 0;
	gsys->mLightSetNum      = 0;

	// iteratively draw all our child nodes
	Node::draw(gfx);

	// draw any whole-game overlays
	Matrix4f orthoMtx;
	gfx.setOrthogonal(orthoMtx.mMtx, AREA_FULL_SCREEN(gfx));
	gfx.useTexture(nullptr, GX_TEXMAP0);

	// if timer is active, print a bunch of graphics trackables to the screen
	if (gsys->mTimerState != TS_Off) {
		gfx.setColour(COLOUR_WHITE, true);
		gfx.texturePrintf(gsys->mConsFont, TIMER_STATE_X, TIMER_STATE_Y + 0 * TIMER_STATE_LINE_HEIGHT, "%d polys = %d pps",
		                  gsys->mPolygonCount, int(gsys->mPolygonCount * gsys->getFrameRate()));
		gfx.texturePrintf(gsys->mConsFont, TIMER_STATE_X, TIMER_STATE_Y + 1 * TIMER_STATE_LINE_HEIGHT, "%d anims", gsys->mAnimatedPolygons);
		gfx.texturePrintf(gsys->mConsFont, TIMER_STATE_X, TIMER_STATE_Y + 2 * TIMER_STATE_LINE_HEIGHT, "%d mats", gsys->mMaterialCount);
		gfx.texturePrintf(gsys->mConsFont, TIMER_STATE_X, TIMER_STATE_Y + 3 * TIMER_STATE_LINE_HEIGHT, "%d disps", gsys->mDispCount);
		gfx.texturePrintf(gsys->mConsFont, TIMER_STATE_X, TIMER_STATE_Y + 4 * TIMER_STATE_LINE_HEIGHT, "%d mtxs",
		                  gsys->mDGXGfx->mNextFreeMatrixIdx);
		gfx.texturePrintf(gsys->mConsFont, TIMER_STATE_X, TIMER_STATE_Y + 5 * TIMER_STATE_LINE_HEIGHT, "%d / %d lighting skips / sets",
		                  gsys->mLightingSkips, gsys->mLightingSets);
		gfx.texturePrintf(gsys->mConsFont, TIMER_STATE_X, TIMER_STATE_Y + 6 * TIMER_STATE_LINE_HEIGHT, "%d light sets", gsys->mLightSetNum);
	}

	// print load text after we finish a section transition (only if it's meant to be visible, or is fading out)
	// NB: the code in retail and the DLL do all the preparation to print, but never actually print the text.
	if (gameflow.mCurrLoadTextAlpha > 0.0f || gameflow.mTargetLoadTextAlpha > 0.0f) {
		gameflow.mLoadTextDisplayTimer -= gsys->getFrameTime();
		if (gameflow.mLoadTextDisplayTimer < 0.0f) {
			gameflow.mTargetLoadTextAlpha = 0.0f;
		}

		gameflow.mCurrLoadTextAlpha += gsys->getFrameTime() * 1.0f * (gameflow.mTargetLoadTextAlpha - gameflow.mCurrLoadTextAlpha);
		if (quickABS(gameflow.mCurrLoadTextAlpha - gameflow.mTargetLoadTextAlpha) < 0.1f) {
			gameflow.mCurrLoadTextAlpha = gameflow.mTargetLoadTextAlpha;
		}

		gfx.setColour(Colour(192, 255, 255, gameflow.mCurrLoadTextAlpha), true);
		gfx.setAuxColour(Colour(192, 192, 255, gameflow.mCurrLoadTextAlpha));
		char loadText[PATH_MAX];
		sprintf(loadText, "load took %.1f secs", gameflow.mLoadTimeSeconds);
#if defined(DEVELOP)
		// this doesn't exist in any build, but clearly something did at one stage.
		gfx.texturePrintf(gsys->mConsFont, 32, 10, loadText);
#endif
	}

	gfx.useTexture(nullptr, GX_TEXMAP0);

	// this is actually for allowing buffer time before transiting between sections, rather than fading anything
	if (gsys->mCurrentFade < gsys->mTargetFade) {
		// "fading in"
		gsys->mCurrentFade += gsys->getFrameTime() * gsys->mFadeRate;
		if (gsys->mCurrentFade > gsys->mTargetFade) {
			gsys->mCurrentFade = gsys->mTargetFade;
		}

	} else if (gsys->mCurrentFade > gsys->mTargetFade) {
		// "fading out"
		gsys->mCurrentFade = gsys->mTargetFade;
		gsys->mCurrentFade -= gsys->getFrameTime() * gsys->mFadeRate;
		if (gsys->mCurrentFade < gsys->mTargetFade) {
			gsys->mCurrentFade = gsys->mTargetFade;
		}
	}

	// draw timers, if enabled
	if (gsys->mTimerState != TS_Off) {
		gsys->mTimer->draw(gfx, gsys->mConsFont);
	}

	gsys->mTimer->stop("cpu draw");
}

/**
 * @brief Base "idle" loop of the game application, initiating all updating, drawing, and transitions.
 *
 * @return Always returns 1.
 */
int PlugPikiApp::idle()
{
#if defined(TARGET_PC)
	static bool firstIdle = true;
	static u32 hostIdleIndex = 0;
	const bool traceHostIdle = hostIdleIndex < 4;
	if (traceHostIdle) OSReport("[pikmin::idle] %u begin heap=%u softReset=%d\n", hostIdleIndex, mHeapIndex, gsys->mSoftResetPending);
#endif
	// use correct heap based on game flow
	// system heap for boot-up, overlay heap for transitions, app heap for all other times
	gsys->setHeap(mHeapIndex);
	gsys->mTimer->newFrame();
	gsys->mTimer->_start("all", false);

	gsys->mIsRendering; // ok.

	// if we have a transition queued, do partial reset without updating
	if (gsys->mSoftResetPending) {
#if defined(TARGET_PC)
		if (firstIdle) OSReport("[pikmin::boot] first idle: processing queued soft reset\n");
#endif
		gsys->detachObjs();
		gsys->mTimer->reset();
		gsys->mSoftResetPending = false;

		softReset();
#if defined(TARGET_PC)
		if (firstIdle) OSReport("[pikmin::boot] first idle: softReset returned\n");
#endif

		// re-attach everything
		PRINT("idle attach\n");
		gsys->attachObjs();
		PRINT("done attaching objs!\n");
#if defined(TARGET_PC)
		if (firstIdle) { OSReport("[pikmin::boot] first idle: soft-reset frame complete\n"); firstIdle = false; }
		if (traceHostIdle) OSReport("[pikmin::idle] %u soft-reset path complete\n", hostIdleIndex);
		++hostIdleIndex;
#endif

		return 1;
	}

	// trigger whole-app update cascade
#if defined(TARGET_PC)
	if (traceHostIdle) OSReport("[pikmin::idle] %u update begin\n", hostIdleIndex);
#endif
	update();
#if defined(TARGET_PC)
	if (traceHostIdle) OSReport("[pikmin::idle] %u update done; beginRender\n", hostIdleIndex);
#endif

	gsys->beginRender();
#if defined(TARGET_PC)
	if (traceHostIdle) OSReport("[pikmin::idle] %u beginRender done; renderall\n", hostIdleIndex);
#endif
	// trigger whole-app rendering cascade
	renderall();
#if defined(TARGET_PC)
	if (traceHostIdle) OSReport("[pikmin::idle] %u renderall done\n", hostIdleIndex);
#endif
	// also render any DVD errors
	if (gsys->mDvdErrorCallback) {
		gsys->mDvdErrorCallback->invoke(*gsys->mDGXGfx);
	}
	gsys->mTimer->start("render", true);
#if defined(TARGET_PC)
	if (traceHostIdle) OSReport("[pikmin::idle] %u doneRender begin\n", hostIdleIndex);
#endif
	gsys->doneRender();
#if defined(TARGET_PC)
	if (traceHostIdle) OSReport("[pikmin::idle] %u doneRender done\n", hostIdleIndex);
#endif
	gsys->mTimer->stop("render");

	// process any messages that have built up this frame
	if (gameflow.mGameInterface) {
		gameflow.mGameInterface->parseMessages();
	}

	gsys->mTimer->_stop("all");

#if defined(TARGET_PC)
	if (traceHostIdle) OSReport("[pikmin::idle] %u waitRetrace begin\n", hostIdleIndex);
#endif
	gsys->waitRetrace();
#if defined(TARGET_PC)
	if (traceHostIdle) OSReport("[pikmin::idle] %u waitRetrace done\n", hostIdleIndex);
#endif
	gsys->setHeap(SYSHEAP_NULL);
#if defined(TARGET_PC)
	if (firstIdle) { OSReport("[pikmin::boot] first idle: regular frame complete\n"); firstIdle = false; }
#endif

	return 1;
}

/**
 * @brief Constructs the root game app and initiates boot setup.
 */
PlugPikiApp::PlugPikiApp()
{
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] PlugPikiApp ctor body begin this=%p\n", this);
#endif
	setName("Piki the Game"); // you sure are buddy

	// initial boot-up - hard reset app
	gsys->setHeap(SYSHEAP_Sys);
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] PlugPikiApp calling hardReset\n");
#endif
	hardReset();
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] PlugPikiApp hardReset returned\n");
#endif

	// set up command stream for linking between "services"
	// (this is unused in the DOL version, but is used in the DLL)
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] allocating AtxCommandStream\n");
#endif
	mCommandStream = new AtxCommandStream(this);
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] AtxCommandStream ready=%p, opening service\n", mCommandStream);
#endif
	if (mCommandStream->open(ATX_SERVICE_APP, 3)) {
		mCommandStream->mPath = Name();
	} else {
		mCommandStream = nullptr;
	}
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] ATX service step complete stream=%p\n", mCommandStream);
#endif

	// default is to print debug timers to the screen, but this is switched off in every game section so it never actually happens
	gsys->mTimerState = TS_On;

	// also do system hard reset
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] entering System::hardReset\n");
#endif
	gsys->hardReset();
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] System::hardReset complete\n");
#endif

	PRINT("*--------------- <%s> after all system setup %.2fk free \n", gsys->getHeap(gsys->mActiveHeapIdx)->mName,
	      gsys->getHeap(gsys->mActiveHeapIdx)->getFree() / 1024.0f);
	gsys->mForcePrint = FALSE;

	// unset heap index - it will be set fresh next frame
	gsys->setHeap(SYSHEAP_NULL);
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] PlugPikiApp ctor complete\n");
#endif
}
