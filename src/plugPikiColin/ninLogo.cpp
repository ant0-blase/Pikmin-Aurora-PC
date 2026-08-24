#include "NinLogoSection.h"

#include "Controller.h"
#include "DebugLog.h"
#include "Dolphin/os.h"
#include "Dolphin/vi.h"
#include "Geometry.h"
#include "Graphics.h"
#include "Menu.h"
#include "Section.h"
#include "gameflow.h"
#include "jaudio/piki_player.h"
#include "sysNew.h"
#include "types.h"
#include "zen/DrawProgre.h"

/**
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @note UNUSED Size: 0000F0
 */
DEFINE_PRINT(nullptr);

/// "Would you like to display in Progressive Scan mode?" screen.
static zen::DrawProgre* progresWindow;
#if defined(TARGET_PC)
static u32 sHostLogoFrames = 0;
static bool sHostLogoCuePlayed = false;
#endif

/**
 * @brief Inner control game section for the boot-up screen, which does more of the labour than its parent `NinLogoSection`.
 *
 * Controls the progressive mode option screen, if required, and sets up while the Nintendo logo is displayed.
 * (It doesn't start the logo, but it does keep it displayed while the progressive scan choice screen is open.)
 *
 * @note Size: 0x50.
 */
struct NinLogoSetupSection : public Node {

	/// Constructs a new control section for progressive scan mode choice.
	NinLogoSetupSection()
	{
		setName("NinLogo section");
		mController   = new Controller(1);
		progresWindow = nullptr;

#if defined(VERSION_GPIP01)
		// no progressive mode for PAL
#else
		if ((VIGetDTVStatus() && OSGetProgressiveMode()) || (VIGetDTVStatus() && gsys->mControllerMgr.keyDown(KBBTN_DPAD_RIGHT))) {
			progresWindow = new zen::DrawProgre();
			progresWindow->start();
		}
#endif
		mActiveDebugMenu                 = nullptr;
#if defined(TARGET_PC)
		// Keep host-only timing outside the legacy object layout. This section is
		// reconstructed from a 32-bit GameCube class, so adding state into the
		// instance is unnecessarily fragile on x86_64.
		sHostLogoFrames = 0;
		sHostLogoCuePlayed = false;
#endif
		gameflow.mGamePrefs.mHasSaveGame = true;
		gsys->setFade(1.0f);
	}

	/// Updates the current frame of boot-up, handling progressive scan choice and transiting to the title screen.
	virtual void update() // _10
	{
#if defined(TARGET_PC)
		OSReport("[pikmin::ninlogo] update begin controller=%p progressive=%p\n", mController, progresWindow);
		if (!sHostLogoCuePlayed) {
			sHostLogoCuePlayed = true;
			OSReport("[pikmin::audio] triggering host Nintendo-logo Pikmin cue\n");
			Jac_PlayOrimaSe(JACORIMA_Unk800C);
		}
#endif
		mController->update();
#if defined(TARGET_PC)
		OSReport("[pikmin::ninlogo] controller update done\n");
#endif

		// there is no debug menu that could exist here, but update it just in case
		if (mActiveDebugMenu) {
			mActiveDebugMenu = mActiveDebugMenu->doUpdate(false);
			return;
		}

		// handle progressive scan mode choice window
		if (progresWindow) {
			zen::DrawProgre::returnStatusFlag scanMode = progresWindow->update(mController);
			if (scanMode == zen::DrawProgre::RETSTATE_Progressive) {
				OSSetProgressiveMode(TRUE);
				gsys->mDGXGfx->mRenderMode = 1;
				gsys->mDGXGfx->videoReset();
				progresWindow = nullptr;
				return;
			}
			if (scanMode == zen::DrawProgre::RETSTATE_Interlaced) {
				OSSetProgressiveMode(FALSE);
				progresWindow = nullptr;
				return;
			}
		} else {
			// On GameCube the Nintendo logo remained visible while the boot/loading
			// path ran in parallel. The native port intentionally removed that GPU
			// worker, so preserve the visible boot logo explicitly on the main thread.
#if defined(TARGET_PC)
			// Titles runs at one VI per update on the native path, so 120 updates is
			// approximately two seconds at 59.94 Hz. Do not use mDeltaTime here: the
			// original boot path relied on work performed by the removed loading GPU
			// thread and therefore has no useful game-time timer of its own.
			++sHostLogoFrames;
			const bool canSkip  = sHostLogoFrames >= 30;
			const bool skipLogo = canSkip && mController->keyClick(KBBTN_A | KBBTN_START);
			if (sHostLogoFrames < 120 && !skipLogo) {
				return;
			}
			OSReport("[pikmin::ninlogo] logo hold complete frames=%u skip=%d; queueing Titles soft reset\n", sHostLogoFrames, skipLogo);
#else
			// transit to title once progressive mode window is complete
#endif
			gameflow.mNextGameSectionID = SECTION_Titles;
			gsys->softReset();
		}
#if defined(TARGET_PC)
		OSReport("[pikmin::ninlogo] update complete softReset=%d next=%d\n", gsys->mSoftResetPending, gameflow.mNextGameSectionID);
#endif
	}

	/**
	 * @brief Renders the current frame of boot-up, mostly handling the progressive scan mode screen.
	 * @param gfx Graphics context for rendering.
	 */
	virtual void draw(Graphics& gfx) // _14
	{
#if defined(TARGET_PC)
		OSReport("[pikmin::ninlogo] draw begin banner=%p fade=%.3f\n", gameflow.mLevelBannerTex, gameflow.mLevelBannerFadeValue);
#endif
		gfx.setViewport(AREA_FULL_SCREEN(gfx));
		gfx.setScissor(AREA_FULL_SCREEN(gfx));
		gfx.setClearColour(COLOUR_TRANSPARENT);
		gfx.clearBuffer(Graphics::ClearBufferFlag::Both, false);

		// draw debug menu or progressive scan choice screen full-screen
		Matrix4f orthoMtx;
		gfx.setOrthogonal(orthoMtx.mMtx, AREA_FULL_SCREEN(gfx));
		gfx.setColour(Colour(255, 255, 64, 255), true);
		gfx.setAuxColour(Colour(255, 0, 64, 255));

		if (mActiveDebugMenu) {
			drawMenu(gfx, mActiveDebugMenu, 1.0f);
		} else if (progresWindow) {
			progresWindow->draw(gfx);
		}

		// keep Nintendo logo open while progressive scan mode choice is going
		gameflow.drawLoadLogo(gfx, false, gameflow.mLevelBannerTex, gameflow.mLevelBannerFadeValue);
#if defined(TARGET_PC)
		OSReport("[pikmin::ninlogo] drawLoadLogo done\n");
#endif

		// either this is a lot of inlines or there's a lot of debug stuff here.
		STACK_PAD_VAR(64);
	}

	/**
	 * @brief Renders the given debug menu, handling any pop-out submenu overlay alphas.
	 * @param gfx Graphics context for rendering.
	 * @param menu Menu to render.
	 * @param fadeFactor Factor to apply to menu alpha (0-1, 0=no alpha, 1=full alpha).
	 */
	void drawMenu(Graphics& gfx, Menu* menu, f32 fadeFactor)
	{
		if (menu->mAlignToParentItem) {
			// create pop-out submenu effect - have parent menu at half alpha behind this menu
			drawMenu(gfx, menu->mParentMenu, 0.5f * fadeFactor);
		}

		menu->draw(gfx, fadeFactor);
	}

	// _00     = VTBL
	// _00-_20 = Node
	u8 _20[0x28 - 0x20];     ///< _20, unknown/unused.
	Controller* mController; ///< _28, active player controller.
	Menu* mActiveDebugMenu;  ///< _2C, current active debug menu (always `nullptr`, since we have none set up).
	u8 _30[0x50 - 0x30];     ///< _30, unknown/unused.
};

/**
 * @brief Initialises progressive scan mode screen game section.
 *
 * Most of the hard work gets farmed out to NinLogoSetupSection above.
 */
void NinLogoSection::init()
{
	Node::init("<NinLogoSection>");
	// make sure debug timer display is off for players
	gsys->mTimerState = TS_Off;

	// do the actual setup and controlling
	add(new NinLogoSetupSection());
}
