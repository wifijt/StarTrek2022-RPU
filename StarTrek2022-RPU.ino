/**************************************************************************
    This pinball code is distributed in the hope that it
    will be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    See <https://www.gnu.org/licenses/>.
*/

#include "RPU_Config.h"
#include "RPU.h"
#include "DropTargets.h"
#include "StarTrek2022.h"
#include "OperatorMenus.h"
#include "AudioHandler.h"
#include "DisplayHandler.h"
#include "LampAnimations.h"
#ifdef RPU_OS_USE_PINNODE_LINK
// PinNode: the installed PinNode library (the link to the node and the PinNodeCtrl_ API)
// and the manifest constants (PinNodeGame.h, generated from pinnode.game.json).
#include <PinNode.h>
#include "PinNodeGame.h"
#endif
#ifdef RPU_OS_USE_ACCESSORY_LAMP_BOARD
#include "ALB-Communication.h"
#endif
#include <EEPROM.h>


#define GAME_MAJOR_VERSION  2022
#define GAME_MINOR_VERSION  2
// Debug text goes to Serial (USB). Leave it off in the machine: a full transmit buffer
// stalls loop(), and the PinNode link needs every pass well under 100 ms.
#define DEBUG_MESSAGES  0

#if (DEBUG_MESSAGES==1)
//#define DEBUG_SHOW_LOOPS_PER_SECOND
#endif

#ifdef RPU_SIMPLIFY_DISPLAY_FOR_7VOLUTION
#define DISPLAY_DASH_STYLE  DISPLAY_DASH_INTERMITTENT_FLASH
#else
#define DISPLAY_DASH_STYLE  DISPLAY_DASH_ROLLING_BLANK
#endif


/*********************************************************************

    Game specific code

*********************************************************************/

// MachineState
//  0 - Attract Mode
//  negative - no longer used
//  positive - game play
boolean InOperatorMenu = false;
char MachineState = 0;
boolean MachineStateChanged = true;
#define MACHINE_STATE_ATTRACT         0
#define MACHINE_STATE_INIT_GAMEPLAY   1
#define MACHINE_STATE_INIT_NEW_BALL   2
#define MACHINE_STATE_NORMAL_GAMEPLAY 4
#define MACHINE_STATE_COUNTDOWN_BONUS 99
#define MACHINE_STATE_BALL_OVER       100
#define MACHINE_STATE_MATCH_MODE      110
#define MACHINE_STATE_DIAGNOSTICS     120

// Indices of EEPROM save locations
#define EEPROM_RPOS_INIT_PROOF_UL                 90

// A value unique to this game code ("ST22"): if the EEPROM doesn't hold it, the
// settings are from some other game and the defaults are loaded.
#define RPOS_INIT_PROOF                           0x53543232

#define EEPROM_BALL_SAVE_BYTE                     100
#define EEPROM_FREE_PLAY_BYTE                     101
#define EEPROM_TILT_WARNING_BYTE                  104
#define EEPROM_AWARD_OVERRIDE_BYTE                105
#define EEPROM_BALLS_OVERRIDE_BYTE                106
#define EEPROM_TOURNAMENT_SCORING_BYTE            107
#define EEPROM_SFX_VOLUME_BYTE                    108
#define EEPROM_MUSIC_VOLUME_BYTE                  109
#define EEPROM_SCROLLING_SCORES_BYTE              110
#define EEPROM_CALLOUTS_VOLUME_BYTE               111
#define EEPROM_CRB_HOLD_TIME                      118
#define EEPROM_TROUGH_EJECT_STRENGTH              131
#define EEPROM_SAUCER_EJECT_STRENGTH              134
#define EEPROM_SLINGSHOT_STRENGTH                 135 
#define EEPROM_POP_BUMPER_STRENGTH                136
#define EEPROM_GAME_RULES_SELECTION               137
#define EEPROM_MATCH_FEATURE_BYTE                 138
#define EEPROM_APP_STARTS_BYTE                    139
#define EEPROM_EXTRA_BALL_SCORE_UL                160
#define EEPROM_SPECIAL_SCORE_UL                   164


// Game modes, in the RPU way: one value per stage, each stage started by
// SetGameMode() and run from ManageGameMode(), which moves on when
// GameModeEndTime passes or the switch handlers call SetGameMode().
// A mission is a group of modes: START (intro), its phases, and FINISH (award).
#define GAME_MODE_SKILL_SHOT                        1
#define GAME_MODE_UNSTRUCTURED_PLAY                 2
// The Doomsday Machine
#define GAME_MODE_DOOMSDAY_START                    10  // intro: music and callout
#define GAME_MODE_DOOMSDAY_SHIELDS                  11  // phase 1: drops locked, spell BALLY
#define GAME_MODE_DOOMSDAY_ATTACK                   12  // phase 2: timed, drop all four targets
#define GAME_MODE_DOOMSDAY_FINISH                   13  // award
// Reserved for the missions still to be built (see docs/RULES.md):
//   20-29 The Trouble with Tribbles        30-39 The City on the Edge of Forever
//   40-49 Space Seed                       50-59 Mirror, Mirror
//   60-69 mini missions (Engine Overload, Self Destruct Sequence)

// Missions. A player's mission in progress carries from ball to ball.
#define MISSION_NONE                      0
#define MISSION_DOOMSDAY                  1
//#define MISSION_TRIBBLES                2
//#define MISSION_CITY_ON_THE_EDGE        3
//#define MISSION_SPACE_SEED              4
//#define MISSION_MIRROR_MIRROR           5
//#define MISSION_ENGINE_OVERLOAD         6   (mini mission)
//#define MISSION_SELF_DESTRUCT           7   (mini mission)

// What a player has done toward qualifying the next mission (MissionQualifiers[])
#define MISSION_QUALIFIER_BALLY           0x01
#define MISSION_QUALIFIER_DROP_BANK       0x02
#define MISSION_QUALIFIERS_FOR_DOOMSDAY   (MISSION_QUALIFIER_BALLY | MISSION_QUALIFIER_DROP_BANK)

#define SKILL_SHOT_BLINK_MS               300     // on and off time of the skill shot lane lamp
#define NEW_GAME_INTRO_MS                 20000   // length of the new game theme (track 1)
#define DOOMSDAY_START_DURATION           0       // ms of intro before phase 1 (room for a light show)
#define DOOMSDAY_ATTACK_DURATION          38000   // ms to drop all four targets
#define DOOMSDAY_FINISH_DURATION          0       // ms of award show before play resumes
#define DOOMSDAY_COMPLETE_AWARD           60000

// Sounds: the track numbers of the 2022 code's WAV Trigger card, unchanged
#define SOUND_EFFECT_NONE                 0
#define SOUND_EFFECT_NEW_GAME             1
#define SOUND_EFFECT_BUMPER_HIT_1         2
#define SOUND_EFFECT_BUMPER_HIT_2         3
#define SOUND_EFFECT_BUMPER_HIT_3         4
#define SOUND_EFFECT_TRANSPORTER          5
#define SOUND_EFFECT_PHOTON_TORPEDO_1     6
#define SOUND_EFFECT_PHOTON_TORPEDO_2     7
#define SOUND_EFFECT_PHOTON_TORPEDO_3     8
#define SOUND_EFFECT_UNLIT_LAMP_2         9
#define SOUND_EFFECT_SLING_SHOT           10
#define SOUND_EFFECT_OUTLANE_UNLIT        14
#define SOUND_EFFECT_SHOOT_AGAIN          15
#define SOUND_EFFECT_PHASER_1             17
#define SOUND_EFFECT_PHASER_2             18
#define SOUND_EFFECT_RED_ALERT            19
#define SOUND_EFFECT_ROTATE_BALLY_1       20
#define SOUND_EFFECT_ROTATE_BALLY_2       21
#define SOUND_EFFECT_ROTATE_BALLY_3       22
#define SOUND_EFFECT_DROP_TARGET_1        23
#define SOUND_EFFECT_DROP_TARGET_2        24
#define SOUND_EFFECT_DROP_TARGET_3        25
#define SOUND_EFFECT_DROP_TARGET_COMPLETE 26
#define SOUND_EFFECT_ADD_CREDIT_1         28
#define SOUND_EFFECT_ADD_CREDIT_2         29
#define SOUND_EFFECT_ADD_CREDIT_3         30
#define SOUND_EFFECT_TOP_TARGET           31
#define SOUND_EFFECT_CENTER_TARGET        32
#define SOUND_EFFECT_ANIMATION_TICK       33
#define SOUND_EFFECT_INNER_LANE           34
#define SOUND_EFFECT_TILT_WARNING         35
#define SOUND_EFFECT_TILT                 36
#define SOUND_EFFECT_OUTTERLANE           37
#define SOUND_EFFECT_WARP                 38
#define SOUND_SPOCK_FASCINATING           39
#define SOUND_EFFECT_BONUS_30K            40
#define SOUND_EFFECT_BONUS_KS             41
#define SOUND_EFFECT_BONUS_OVER           42
#define SOUND_EFFECT_EXTRA_BALL           43
#define SOUND_EFFECT_KICKER_WATCH         44
#define SOUND_EFFECT_BALL_OVER            45
#define SOUND_EFFECT_GAME_OVER            46
#define SOUND_EFFECT_MACHINE_START        47
#define SOUND_EFFECT_SKILL_SHOT           48
#define SOUND_EFFECT_MATCH_SPIN           49
#define SOUND_EFFECT_ROLLOVER             50
#define SOUND_EFFECT_PLACEHOLDER_LETTER   51
#define SOUND_EFFECT_SKILL_SHOT_ALT       52
#define SOUND_EFFECT_BACKGROUND_DRONE     89

// Voice callouts (queued, with the music ducked under them)
#define SOUND_EFFECT_VP_VOICE_NOTIFICATIONS_START   100
#define SOUND_EFFECT_ADD_PLAYER_1         100
#define SOUND_EFFECT_ADD_PLAYER_2         (SOUND_EFFECT_ADD_PLAYER_1+1)
#define SOUND_EFFECT_ADD_PLAYER_3         (SOUND_EFFECT_ADD_PLAYER_1+2)
#define SOUND_EFFECT_ADD_PLAYER_4         (SOUND_EFFECT_ADD_PLAYER_1+3)
#define SOUND_EFFECT_PLAYER_1_UP          104
#define SOUND_EFFECT_PLAYER_2_UP          (SOUND_EFFECT_PLAYER_1_UP+1)
#define SOUND_EFFECT_PLAYER_3_UP          (SOUND_EFFECT_PLAYER_1_UP+2)
#define SOUND_EFFECT_PLAYER_4_UP          (SOUND_EFFECT_PLAYER_1_UP+3)
#define SOUND_EFFECT_DOOMSDAY_PHASE1      108
#define SOUND_EFFECT_DOOMSDAY_PHASE2      109
#define SOUND_EFFECT_DOOMSDAY_COMPLETE    110
#define NUM_VOICE_NOTIFICATIONS           11

// Music
#define SOUND_EFFECT_BACKGROUND_1         201
#define SOUND_EFFECT_BACKGROUND_2         202
#define SOUND_EFFECT_BACKGROUND_3         203
#define SOUND_EFFECT_BACKGROUND_4         204
#define SOUND_EFFECT_BACKGROUND_5         205
#define SOUND_EFFECT_BACKGROUND_6         206
#define SOUND_DOOMSDAY_INTRO              220
#define SOUND_DOOMSDAY_LEVEL_1            221
#define SOUND_DOOMSDAY_LEVEL_2            222

// The operator menu's rules callouts (not on the 2022 sound card: silent until recorded)
#define SOUND_EFFECT_VP_GAME_RULES_EASY               423
#define SOUND_EFFECT_VP_GAME_RULES_MEDIUM             424
#define SOUND_EFFECT_VP_GAME_RULES_HARD               425

#define SOUND_EFFECT_DIAG_START                   1900
#define SOUND_EFFECT_DIAG_CREDIT_RESET_BUTTON     1900
#define SOUND_EFFECT_DIAG_SELECTOR_SWITCH_ON      1901
#define SOUND_EFFECT_DIAG_SELECTOR_SWITCH_OFF     1902
#define SOUND_EFFECT_DIAG_STARTING_ORIGINAL_CODE  1903
#define SOUND_EFFECT_DIAG_STARTING_NEW_CODE       1904
#define SOUND_EFFECT_DIAG_ORIGINAL_CPU_DETECTED   1905
#define SOUND_EFFECT_DIAG_ORIGINAL_CPU_RUNNING    1906
#define SOUND_EFFECT_DIAG_PROBLEM_PIA_U10         1907
#define SOUND_EFFECT_DIAG_PROBLEM_PIA_U11         1908
#define SOUND_EFFECT_DIAG_PROBLEM_PIA_1           1909
#define SOUND_EFFECT_DIAG_PROBLEM_PIA_2           1910
#define SOUND_EFFECT_DIAG_PROBLEM_PIA_3           1911
#define SOUND_EFFECT_DIAG_PROBLEM_PIA_4           1912
#define SOUND_EFFECT_DIAG_PROBLEM_PIA_5           1913
#define SOUND_EFFECT_DIAG_STARTING_DIAGNOSTICS    1914

// The most the bonus lamps can show: 20K + 10K + 9K
#define MAX_DISPLAY_BONUS     39
#define TILT_WARNING_DEBOUNCE_TIME        1000
#define BALL_SAVE_GRACE_PERIOD            3000

/*********************************************************************

    Machine state and options

*********************************************************************/
byte Credits = 0;
byte BallSaveNumSeconds = 0;
byte MaximumCredits = 40;
byte BallsPerGame = 3;
byte ScoreAwardReplay = 0;
byte MusicVolume = 6;
byte SoundEffectsVolume = 8;
byte CalloutsVolume = 10;
byte ChuteCoinsInProgress[3];
byte TotalBallsLoaded = 1;
byte TimeRequiredToResetGame = 1;
boolean FreePlayMode = false;
boolean MatchEnabled = true;
boolean HighScoreReplay = true;
boolean MatchFeature = true;
boolean TournamentScoring = false;
boolean ScrollingScores = true;
boolean AppStartsEnabled = true;    // a start from the PinNode app is taken like the start button
boolean GIInOriginal = true;
unsigned long ExtraBallValue = 0;
unsigned long SpecialValue = 0;
unsigned long CurrentTime = 0;
unsigned long HighScore = 0;
unsigned long AwardScores[3];
unsigned long CreditResetPressStarted = 0;
unsigned long OperatorSwitchPressStarted = 0;

#ifdef RPU_OS_USE_ACCESSORY_LAMP_BOARD
unsigned long ALBWatchdogResetLastSent;
#endif

#define NUM_CPC_PAIRS 9
boolean CPCSelectionsHaveBeenRead = false;
byte CPCPairs[NUM_CPC_PAIRS][2] = {
  {1, 5},
  {1, 4},
  {1, 3},
  {1, 2},
  {1, 1},
  {2, 3},
  {2, 1},
  {3, 1},
  {4, 1}
};
byte CPCSelection[3];

AudioHandler  Audio;
OperatorMenus Menus;

#ifdef RPU_OS_USE_ACCESSORY_LAMP_BOARD
AccessoryLampBoard TopperALB;
#endif


/*********************************************************************

    Game State

*********************************************************************/
byte CurrentPlayer = 0;
byte CurrentBallInPlay = 1;
byte CurrentNumPlayers = 0;
byte NumberOfBallsLocked;
byte NumberOfBallsInPlay;
byte Bonus[RPU_NUMBER_OF_PLAYERS_ALLOWED];
byte BonusX[RPU_NUMBER_OF_PLAYERS_ALLOWED];
byte GameMode = GAME_MODE_SKILL_SHOT;
byte LastGameMode = 0;
byte MaxTiltWarnings = 2;
byte NumTiltWarnings = 0;
byte CurrentAchievements[RPU_NUMBER_OF_PLAYERS_ALLOWED];
byte NumberOfBallSavesRemaining;

boolean SamePlayerShootsAgain = false;
boolean BallSaveUsed = false;
boolean SpecialAvailable = false;
boolean SpecialCollected = false;
boolean TimersPaused = true;

unsigned long CurrentScores[RPU_NUMBER_OF_PLAYERS_ALLOWED];
unsigned long BallFirstSwitchHitTime = 0;
unsigned long BallTimeInTrough = 0;
unsigned long GameModeStartTime = 0;
unsigned long GameModeEndTime = 0;
unsigned long LastTiltWarningTime;
unsigned long PlayfieldMultiplier;
unsigned long LastTimeThroughLoop;
unsigned long LastSwitchHitTime;
unsigned long BallSaveEndTime;


/*********************************************************************

    Game Specific State Variables

*********************************************************************/
byte GameRulesSelection;
byte BallServeSolenoidStrength;
byte SaucerSolenoidStrength;

#define GAME_RULES_EASY         1
#define GAME_RULES_MEDIUM       2
#define GAME_RULES_HARD         3
#define GAME_RULES_PROGRESSIVE  4
#define GAME_RULES_CUSTOM       5

unsigned long PlayfieldMultiplierTimeLeft;
unsigned long BonusChanged;
unsigned long BonusXAnimationStart;
unsigned long LastTimeBallServed;
unsigned long LastTimeSaucerSeen;

#define DROP_TARGET_RESET_STRENGTH    12
#define KNOCKER_SOLENOID_STRENGTH     3

// The four-target bank
DropTargetBank DropTargets(4, 1, DROP_TARGET_TYPE_BLY_1, DROP_TARGET_RESET_STRENGTH);

byte PlayerUpLamps[4] = {LAMP_HEAD_PLAYER_1_UP, LAMP_HEAD_PLAYER_2_UP, LAMP_HEAD_PLAYER_3_UP, LAMP_HEAD_PLAYER_4_UP};

// ---- Star Trek state, per player (kept from ball to ball)
byte BallyStatus[RPU_NUMBER_OF_PLAYERS_ALLOWED];          // B-A-L-L-Y letters lit (bits 16,8,4,2,1)
byte HSBonusIDX[RPU_NUMBER_OF_PLAYERS_ALLOWED];           // hyperspace ladder, 0-6
byte MissionInProgress[RPU_NUMBER_OF_PLAYERS_ALLOWED];    // MISSION_*
byte MissionQualifiers[RPU_NUMBER_OF_PLAYERS_ALLOWED];    // MISSION_QUALIFIER_* bits
byte MissionsCompleted[RPU_NUMBER_OF_PLAYERS_ALLOWED];    // one bit per mission, (1 << (mission - 1))
unsigned short AchievementsAwarded[RPU_NUMBER_OF_PLAYERS_ALLOWED];   // one bit per ACH_ index

// ---- Star Trek state, per ball
byte SaucerLetterIDX = 0;         // which letter the saucer spots
byte DTBonusIDX = 1;              // drop target ladder
byte DTBonusValue[5] = {10, 25, 50, 75, 75};   // thousands, by ladder step
byte DTBonusMultiplier = 1;
byte ToplaneProgress;             // which top lanes have been made this ball (bit 0: B, bit 1: A)
byte SkillShotLane;               // 0: B, 1: A
boolean ExtraBallCollected = false;
boolean LaneLightPhase = false;
boolean HSEBQualified = false;
boolean BonusCollectedAtGate = false;   // the ball return gate pays the bonus once per ball
boolean BallSaveStarted = false;        // the ball save runs once per ball
unsigned long IntroMusicEndTime = 0;    // the new game theme is playing until then (0: it isn't)
unsigned long LastSaucerHitTime;
unsigned long SaucerKickTime;
unsigned long BallyAnimationBegin;
unsigned long HSrollovertimer;
unsigned long HyperSpaceExtraBonus;   // when the hyperspace bonus window opened (0: closed)
unsigned long HSLampAnimationDelay;
byte HSLampLit;


/******************************************************

   Adjustments Serialization

*/
void SetAllParameterDefaults() {

  // In the event that the EEPROM has not been initialized,
  // these are the values that will be used
  HighScore = 10000;
  Credits = 4;
  FreePlayMode = false;
  BallSaveNumSeconds = 15;
  MusicVolume = 10;
  SoundEffectsVolume = 10;
  CalloutsVolume = 10;
  AwardScores[0] = 200000;
  AwardScores[1] = 400000;
  AwardScores[2] = 600000;
  TournamentScoring = false;
  MaxTiltWarnings = 2;
  ScoreAwardReplay = 0x07;
  BallsPerGame = 3;
  ScrollingScores = true;
  MatchFeature = true;
  AppStartsEnabled = true;
  ExtraBallValue = 20000;
  SpecialValue = 40000;
  TimeRequiredToResetGame = 2;
  CPCSelection[0] = 4;
  CPCSelection[1] = 4;
  CPCSelection[2] = 4;

  // EASY / MEDIUM / HARD rules
  GameRulesSelection = GAME_RULES_MEDIUM;

  BallServeSolenoidStrength = 4;
  SaucerSolenoidStrength = 5;
}


// CUSTOMIZATION TODO:
// Load in defaults for easy / medium / hard rules
boolean LoadRuleDefaults(byte ruleLevel) {
  // This function just puts the rules in RAM.
  // It's up to the caller to ensure that they're 
  // written to EEPROM when desired (with WriteParameters)
  if (ruleLevel==GAME_RULES_EASY) {
    BallSaveNumSeconds = 20;
    // Game rules variables initialized to EASY here:
  } else if (ruleLevel==GAME_RULES_MEDIUM) {
    BallSaveNumSeconds = 10;
    // Game rules variables initialized to MEDIUM here:
  } else if (ruleLevel==GAME_RULES_HARD) {
    BallSaveNumSeconds = 0;
    // Game rules variables initialized to HARD here:
  } else {
    return false;
  }

  return true;
}


void WriteParameters(boolean onlyWriteRulesParameters = true) {
  if (!onlyWriteRulesParameters) {
    RPU_WriteULToEEProm(RPU_HIGHSCORE_EEPROM_START_BYTE, HighScore);
    RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
    RPU_WriteByteToEEProm(RPU_CPC_CHUTE_1_SELECTION_BYTE, CPCSelection[0]);
    RPU_WriteByteToEEProm(RPU_CPC_CHUTE_2_SELECTION_BYTE, CPCSelection[1]);
    RPU_WriteByteToEEProm(RPU_CPC_CHUTE_3_SELECTION_BYTE, CPCSelection[2]);
        
    RPU_WriteByteToEEProm(EEPROM_FREE_PLAY_BYTE, FreePlayMode);
    RPU_WriteByteToEEProm(EEPROM_MUSIC_VOLUME_BYTE, MusicVolume);
    RPU_WriteByteToEEProm(EEPROM_SFX_VOLUME_BYTE, SoundEffectsVolume);
    RPU_WriteByteToEEProm(EEPROM_CALLOUTS_VOLUME_BYTE, CalloutsVolume);

    RPU_WriteULToEEProm(RPU_AWARD_SCORE_1_EEPROM_START_BYTE, AwardScores[0]);
    RPU_WriteULToEEProm(RPU_AWARD_SCORE_2_EEPROM_START_BYTE, AwardScores[1]);
    RPU_WriteULToEEProm(RPU_AWARD_SCORE_3_EEPROM_START_BYTE, AwardScores[2]);

    RPU_WriteByteToEEProm(EEPROM_TOURNAMENT_SCORING_BYTE, TournamentScoring);
    RPU_WriteByteToEEProm(EEPROM_AWARD_OVERRIDE_BYTE, ScoreAwardReplay);
    RPU_WriteByteToEEProm(EEPROM_BALLS_OVERRIDE_BYTE, BallsPerGame);
    RPU_WriteByteToEEProm(EEPROM_SCROLLING_SCORES_BYTE, ScrollingScores);
    RPU_WriteByteToEEProm(EEPROM_MATCH_FEATURE_BYTE, MatchFeature);
    RPU_WriteByteToEEProm(EEPROM_APP_STARTS_BYTE, AppStartsEnabled);
    
    RPU_WriteULToEEProm(EEPROM_EXTRA_BALL_SCORE_UL, ExtraBallValue);
    RPU_WriteULToEEProm(EEPROM_SPECIAL_SCORE_UL, SpecialValue);
    RPU_WriteByteToEEProm(EEPROM_CRB_HOLD_TIME, TimeRequiredToResetGame);

    // Set baseline for audits
    // (the coin counters are 4 bytes each: writing one byte leaves 0xFFFFFF00 in a fresh EEPROM)
    RPU_WriteULToEEProm(RPU_CHUTE_1_COINS_START_BYTE, 0);
    RPU_WriteULToEEProm(RPU_CHUTE_2_COINS_START_BYTE, 0);
    RPU_WriteULToEEProm(RPU_CHUTE_3_COINS_START_BYTE, 0);
    RPU_WriteByteToEEProm(EEPROM_GAME_RULES_SELECTION, GameRulesSelection);
    RPU_WriteULToEEProm(RPU_TOTAL_PLAYS_EEPROM_START_BYTE, 0);
    RPU_WriteULToEEProm(RPU_TOTAL_REPLAYS_EEPROM_START_BYTE, 0);
    RPU_WriteULToEEProm(RPU_TOTAL_HISCORE_BEATEN_START_BYTE, 0);
  }

  RPU_WriteByteToEEProm(EEPROM_BALL_SAVE_BYTE, BallSaveNumSeconds);
  RPU_WriteByteToEEProm(EEPROM_TILT_WARNING_BYTE, MaxTiltWarnings); 
  
  // CUSTOMIZATION TODO:
  // Write any custom parameters to EEPROM here
  RPU_WriteByteToEEProm(EEPROM_TROUGH_EJECT_STRENGTH, BallServeSolenoidStrength);
  RPU_WriteByteToEEProm(EEPROM_SAUCER_EJECT_STRENGTH, SaucerSolenoidStrength);
}

void ReadStoredParameters() {
  for (byte count = 0; count < 3; count++) {
    ChuteCoinsInProgress[count] = 0;
  }

  // The first time the EEPROM has been written with good values for this game,
  // the EEPROM_RPOS_INIT_PROOF_UL will be written to a known state (RPOS_INIT_PROOF)
  // if that value hasn't been written, then we load defaults and save them to EEPROM.
  // This should only happen the first time a device is run with this game code.
  unsigned long RPUProofValue = RPU_ReadULFromEEProm(EEPROM_RPOS_INIT_PROOF_UL, 0);
  if (RPUProofValue!=RPOS_INIT_PROOF) {
    // Doesn't look like this memory has been initialized
    RPU_WriteULToEEProm(EEPROM_RPOS_INIT_PROOF_UL, RPOS_INIT_PROOF);
    SetAllParameterDefaults();
    WriteParameters(false);
  } else {

    // Read machine settings
    HighScore = RPU_ReadULFromEEProm(RPU_HIGHSCORE_EEPROM_START_BYTE, 10000);
    Credits = RPU_ReadByteFromEEProm(RPU_CREDITS_EEPROM_BYTE);
    if (Credits > MaximumCredits) Credits = MaximumCredits;
  
    FreePlayMode = ReadSetting(EEPROM_FREE_PLAY_BYTE, false, true);
    MusicVolume = ReadSetting(EEPROM_MUSIC_VOLUME_BYTE, 10, 10);
    SoundEffectsVolume = ReadSetting(EEPROM_SFX_VOLUME_BYTE, 10, 10);
    CalloutsVolume = ReadSetting(EEPROM_CALLOUTS_VOLUME_BYTE, 10, 10);
    Audio.SetMusicVolume(MusicVolume);
    Audio.SetSoundFXVolume(SoundEffectsVolume);
    Audio.SetNotificationsVolume(CalloutsVolume);

    AwardScores[0] = RPU_ReadULFromEEProm(RPU_AWARD_SCORE_1_EEPROM_START_BYTE);
    AwardScores[1] = RPU_ReadULFromEEProm(RPU_AWARD_SCORE_2_EEPROM_START_BYTE);
    AwardScores[2] = RPU_ReadULFromEEProm(RPU_AWARD_SCORE_3_EEPROM_START_BYTE);
  
    TournamentScoring = ReadSetting(EEPROM_TOURNAMENT_SCORING_BYTE, false, true);
    ScoreAwardReplay = ReadSetting(EEPROM_AWARD_OVERRIDE_BYTE, 0x03, 0x07);
    BallsPerGame = ReadSetting(EEPROM_BALLS_OVERRIDE_BYTE, 3, 10);
    ScrollingScores = ReadSetting(EEPROM_SCROLLING_SCORES_BYTE, true, true);
    MatchFeature = ReadSetting(EEPROM_MATCH_FEATURE_BYTE, true, true);
    AppStartsEnabled = ReadSetting(EEPROM_APP_STARTS_BYTE, true, true);

    CPCSelection[0] = ReadSetting(RPU_CPC_CHUTE_1_SELECTION_BYTE, 4, 8);
    CPCSelection[1] = ReadSetting(RPU_CPC_CHUTE_2_SELECTION_BYTE, 4, 8);
    CPCSelection[2] = ReadSetting(RPU_CPC_CHUTE_3_SELECTION_BYTE, 4, 8);
    CPCSelectionsHaveBeenRead = true;

    ExtraBallValue = RPU_ReadULFromEEProm(EEPROM_EXTRA_BALL_SCORE_UL);
    if (ExtraBallValue % 1000 || ExtraBallValue > 1000000) ExtraBallValue = 20000;
  
    SpecialValue = RPU_ReadULFromEEProm(EEPROM_SPECIAL_SCORE_UL);
    if (SpecialValue % 1000 || SpecialValue > 1000000) SpecialValue = 40000;
  
    TimeRequiredToResetGame = ReadSetting(EEPROM_CRB_HOLD_TIME, 1, 99);
    if (TimeRequiredToResetGame > 3 && TimeRequiredToResetGame != 99) TimeRequiredToResetGame = 1;
    
    // Read game rules
    GameRulesSelection = ReadSetting(EEPROM_GAME_RULES_SELECTION, GAME_RULES_MEDIUM, GAME_RULES_CUSTOM);

    BallSaveNumSeconds = ReadSetting(EEPROM_BALL_SAVE_BYTE, 15, 20);  
    MaxTiltWarnings = ReadSetting(EEPROM_TILT_WARNING_BYTE, 2, 3);
    
    BallServeSolenoidStrength = ReadSetting(EEPROM_TROUGH_EJECT_STRENGTH, 4, 10);
    if (BallServeSolenoidStrength < 3) BallServeSolenoidStrength = 4;
    SaucerSolenoidStrength = ReadSetting(EEPROM_SAUCER_EJECT_STRENGTH, 5, 15);
  }
}



byte GetCPCSelection(byte chuteNumber) {
  if (chuteNumber>2) return 0xFF;

  if (CPCSelectionsHaveBeenRead==false) {
    CPCSelection[0] = RPU_ReadByteFromEEProm(RPU_CPC_CHUTE_1_SELECTION_BYTE);
    if (CPCSelection[0]>=NUM_CPC_PAIRS) {
      CPCSelection[0] = 4;
      RPU_WriteByteToEEProm(RPU_CPC_CHUTE_1_SELECTION_BYTE, 4);
    }
    CPCSelection[1] = RPU_ReadByteFromEEProm(RPU_CPC_CHUTE_2_SELECTION_BYTE);  
    if (CPCSelection[1]>=NUM_CPC_PAIRS) {
      CPCSelection[1] = 4;
      RPU_WriteByteToEEProm(RPU_CPC_CHUTE_2_SELECTION_BYTE, 4);
    }
    CPCSelection[2] = RPU_ReadByteFromEEProm(RPU_CPC_CHUTE_3_SELECTION_BYTE);  
    if (CPCSelection[2]>=NUM_CPC_PAIRS) {
      CPCSelection[2] = 4;
      RPU_WriteByteToEEProm(RPU_CPC_CHUTE_3_SELECTION_BYTE, 4);
    }
    CPCSelectionsHaveBeenRead = true;
  }
  
  return CPCSelection[chuteNumber];
}


byte GetCPCCoins(byte cpcSelection) {
  if (cpcSelection>=NUM_CPC_PAIRS) return 1;
  return CPCPairs[cpcSelection][0];
}


byte GetCPCCredits(byte cpcSelection) {
  if (cpcSelection>=NUM_CPC_PAIRS) return 1;
  return CPCPairs[cpcSelection][1];
}


void QueueDIAGNotification(unsigned short notificationNum) {
  // This is optional, but the machine can play an audio message at boot
  // time to indicate any errors and whether it's going to boot to original
  // or new code.
  //Audio.QueuePrioritizedNotification(notificationNum, 0, 10, CurrentTime);
  (void)notificationNum;
}

// CUSTOMIZATION TODO:
// This function only needs to be updated if the display
// numbers (in test mode) don't match the indices.
// In most games, the operator friendly numbers are just
// the display numbers + 1
// I'm doing this as a function instead of an array because 
// memory is short and spending 44 bytes on a converstion array
// seems wasteful when the board has tons and tons of code space.
// There's a way to store this data in code space and then convert
// it when needed, but that's slow compared to this (ugly) method.
byte LampConvertDisplayNumberToIndex(byte displayNumber) {

  if (displayNumber<60) return displayNumber + 1;
  return OPERATOR_MENU_VALUE_OUT_OF_RANGE;
}


// CUSTOMIZATION TODO:
// Fill in the "operator friendly" solenoid number in the "case" 
// (this is the number reported in the game manual)
// and then the hardware index in the return (from your defines)
unsigned short SolenoidConvertDisplayNumberToIndex(byte displayNumber) {
  // The test number is the solenoid's index + 0 (the numbers of StarTrek2022.h).
  // TODO: renumber to match the solenoid test numbers in the Bally manual (docs/).
  switch (displayNumber) {
    case  5: return SOL_KNOCKER;
    case  6: return SOL_OUTHOLE;
    case  7: return SOL_SAUCER;
    case  8: return SOL_LEFT_BUMPER;
    case  9: return SOL_RIGHT_BUMPER;
    case 10: return SOL_CENTER_BUMPER;
    case 11: return SOL_LEFT_SLINGSHOT;
    case 12: return SOL_RIGHT_SLINGSHOT;
    case 13: return SOL_DROP_TARGET_RESET;
    case 15: return 0x4000; // Flipper Mute
    case  0: case  1: case  2: case  3: case  4: case 14: case 16: case 17: case 18: case 19:
      return OPERATOR_MENU_VALUE_UNUSED;
    default: return OPERATOR_MENU_VALUE_OUT_OF_RANGE;
  }
}

// CUSTOMIZATION TODO:
// In the operator menu's test mode, these are the strengths
// that will be used 
byte SolenoidConvertDisplayNumberToTestStrength(byte displayNumber) {
  switch (displayNumber) {
    case  5: return KNOCKER_SOLENOID_STRENGTH;
    case  6: return BallServeSolenoidStrength;
    case  7: return SaucerSolenoidStrength;
    case  8: case  9: case 10: return 3;
    case 11: case 12: return 4;
    case 13: return DROP_TARGET_RESET_STRENGTH;
    case 15: return 4;
    case  0: case  1: case  2: case  3: case  4: case 14: case 16: case 17: case 18: case 19:
      return OPERATOR_MENU_VALUE_UNUSED;
    default: return OPERATOR_MENU_VALUE_OUT_OF_RANGE;
  }
}




#ifdef RPU_OS_USE_PINNODE_LINK
////////////////////////////////////////////////////////////////////////////
//
//  PinNode tables
//    The app's machine options are the operator adjustments below: the same
//    EEPROM bytes the operator menu edits. The position in each table is the
//    index in pinnode.game.json (SET_* / AUD_* in PinNodeGame.h), so keep the
//    two in step and never reorder a released index. A difficulty setting
//    (the manifest's "difficulty") can't be changed while a tournament holds
//    the machine's settings.
//
////////////////////////////////////////////////////////////////////////////
const PinNodeSetting PinNodeSettingsTable[] = {
  // eeprom                              size  min  max        scale liveSafe difficulty step
  {EEPROM_FREE_PLAY_BYTE,                  1,  0,   1,          1,  false, false, 0},  // SET_FREE_PLAY
  {EEPROM_BALL_SAVE_BYTE,                  1,  0,   20,         1,  false, true , 0},  // SET_BALL_SAVE
  {EEPROM_TILT_WARNING_BYTE,               1,  0,   2,          1,  false, true , 0},  // SET_TILT_WARNINGS
  {EEPROM_MUSIC_VOLUME_BYTE,               1,  0,   10,         1,  true , false, 0},  // SET_MUSIC_VOLUME
  {EEPROM_SFX_VOLUME_BYTE,                 1,  0,   10,         1,  true , false, 0},  // SET_SFX_VOLUME
  {EEPROM_CALLOUTS_VOLUME_BYTE,            1,  0,   10,         1,  true , false, 0},  // SET_CALLOUTS_VOLUME
  {EEPROM_BALLS_OVERRIDE_BYTE,             1,  3,   10,         1,  false, true , 0},  // SET_BALLS_PER_GAME
  {EEPROM_TOURNAMENT_SCORING_BYTE,         1,  0,   1,          1,  false, true , 0},  // SET_TOURNAMENT_SCORING
  {EEPROM_EXTRA_BALL_SCORE_UL,             4,  0,   99999990,   1,  false, true , 0},  // SET_EXTRA_BALL_VALUE
  {EEPROM_SPECIAL_SCORE_UL,                4,  0,   99999990,   1,  false, true , 0},  // SET_SPECIAL_VALUE
  {EEPROM_CRB_HOLD_TIME,                   1,  0,   99,         1,  false, false, 0},  // SET_RESET_HOLD_TIME
  {RPU_AWARD_SCORE_1_EEPROM_START_BYTE,    4,  0,   99999990,   1,  false, false, 0},  // SET_REPLAY_SCORE_1
  {RPU_AWARD_SCORE_2_EEPROM_START_BYTE,    4,  0,   99999990,   1,  false, false, 0},  // SET_REPLAY_SCORE_2
  {RPU_AWARD_SCORE_3_EEPROM_START_BYTE,    4,  0,   99999990,   1,  false, false, 0},  // SET_REPLAY_SCORE_3
  {EEPROM_AWARD_OVERRIDE_BYTE,             1,  0,   7,          1,  false, false, 0},  // SET_SCORE_AWARD_REPLAY
  {EEPROM_SCROLLING_SCORES_BYTE,           1,  0,   1,          1,  true , false, 0},  // SET_SCROLLING_SCORES
  {RPU_CPC_CHUTE_1_SELECTION_BYTE,         1,  0,   8,          1,  false, false, 0},  // SET_COINS_PER_CREDIT_1
  {RPU_CPC_CHUTE_2_SELECTION_BYTE,         1,  0,   8,          1,  false, false, 0},  // SET_COINS_PER_CREDIT_2
  {RPU_CPC_CHUTE_3_SELECTION_BYTE,         1,  0,   8,          1,  false, false, 0},  // SET_COINS_PER_CREDIT_3
  {EEPROM_MATCH_FEATURE_BYTE,              1,  0,   1,          1,  false, false, 0},  // SET_MATCH_FEATURE
  {EEPROM_GAME_RULES_SELECTION,            1,  1,   5,          1,  false, true , 0},  // SET_GAME_RULES
  {EEPROM_TROUGH_EJECT_STRENGTH,           1,  3,   10,         1,  false, false, 0},  // SET_TROUGH_EJECT_STRENGTH
  {EEPROM_SAUCER_EJECT_STRENGTH,           1,  5,   15,         1,  false, false, 0},  // SET_SAUCER_EJECT_STRENGTH
  {EEPROM_APP_STARTS_BYTE,                 1,  0,   1,          1,  false, false, 0},  // SET_APP_STARTS
};

const PinNodeAudit PinNodeAuditsTable[] = {
  {RPU_TOTAL_PLAYS_EEPROM_START_BYTE,   4},  // AUD_TOTAL_PLAYS
  {RPU_CHUTE_1_COINS_START_BYTE,        4},  // AUD_CHUTE_1_COINS
  {RPU_CHUTE_2_COINS_START_BYTE,        4},  // AUD_CHUTE_2_COINS
  {RPU_CHUTE_3_COINS_START_BYTE,        4},  // AUD_CHUTE_3_COINS
  {RPU_TOTAL_REPLAYS_EEPROM_START_BYTE, 4},  // AUD_TOTAL_REPLAYS
  {RPU_TOTAL_HISCORE_BEATEN_START_BYTE, 4},  // AUD_HIGH_SCORE_BEATEN
};

// The manifest hash in PinNodeGame.h is 16 hex characters; HELLO carries it as 8 bytes
static void PinNodeManifestBytes(byte *hashBytes) {
  const char *hex = PINNODE_MANIFEST_HASH;
  for (byte count = 0; count < 16; count++) {
    char c = hex[count];
    byte nibble = (c >= 'a') ? (c - 'a' + 10) : (c >= 'A') ? (c - 'A' + 10) : (c - '0');
    if (count % 2 == 0) hashBytes[count / 2] = nibble << 4;
    else hashBytes[count / 2] |= nibble;
  }
}


// The app changed a setting: it's in EEPROM already, so re-read as after the operator menu
static void PinNodeSettingsApplied() {
  byte musicVolumeBefore = MusicVolume;
  CPCSelectionsHaveBeenRead = false;
  ReadStoredParameters();

  // A new music volume only reaches the WAV Trigger when a track starts, so the music
  // that's playing is started again at the new volume (or stopped, at volume 0).
  if (MusicVolume != musicVolumeBefore && Audio.GetBackgroundSong() != BACKGROUND_TRACK_NONE) {
    if (MusicVolume == 0) Audio.StopAllMusic();
    else Audio.PlayBackgroundSong(Audio.GetBackgroundSong());
  }
}


////////////////////////////////////////////////////////////////////////////
//
//  Operator actions from the phone (SPEC-operator-actions)
//    The tests and one-shot operations of the operator menu, as plain
//    functions a request from the app can start and stop. They don't touch
//    the coin-door menus (OperatorMenus).
//      - a test runs only in MACHINE_STATE_DIAGNOSTICS, which the phone enters
//        (from attract only) and leaves with the two diagnostics actions;
//      - a one-shot runs in attract or diagnostics;
//      - nothing runs during a game, or while the coin-door menu is open.
//    The position in PinNodeActionsTable is the index in the manifest's
//    actions[] list (the ACT_ constants): keep the two in step.
//
////////////////////////////////////////////////////////////////////////////
// The action numbers are the ACT_ constants of PinNodeGame.h (from the manifest)
#define REMOTE_TEST_NONE            0xFF

const PinNodeAction PinNodeActionsTable[] = {
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // lamp-test
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // display-test
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // solenoid-test
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // switch-test
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // sound-test
  {PINNODE_ACTION_LOCAL},                                                 // eject-balls
  {PINNODE_ACTION_CONFIRM},                                               // clear-audits
  {PINNODE_ACTION_CONFIRM | PINNODE_ACTION_LOCK},                         // factory-defaults
  {PINNODE_ACTION_CONFIRM | PINNODE_ACTION_LOCK},                         // preset-easy
  {PINNODE_ACTION_CONFIRM | PINNODE_ACTION_LOCK},                         // preset-medium
  {PINNODE_ACTION_CONFIRM | PINNODE_ACTION_LOCK},                         // preset-hard
  {PINNODE_ACTION_CONFIRM | PINNODE_ACTION_LOCK},                         // preset-progressive
  {PINNODE_ACTION_LOCAL | PINNODE_ACTION_ENTER_DIAG},                     // enter-diagnostics
  {PINNODE_ACTION_LOCAL | PINNODE_ACTION_EXIT_DIAG},                      // exit-diagnostics
  {PINNODE_ACTION_CONFIRM},                                               // clear-credits
};

#define SOLENOID_TEST_PERIOD_MS           2000
#define SWITCH_TEST_NUM_SWITCHES          40      // the switches polled for open/closed changes

byte RemoteTest = REMOTE_TEST_NONE;            // the test running now
uint16_t RemoteTestReqId = 0;
long RemoteTestArg = 0;
long RemoteTestValue = 0;                 // what the app shows: lamp, digit, solenoid, closures, track
unsigned long RemoteTestLastStep = 0;
byte RemoteSwitchStates[(SWITCH_TEST_NUM_SWITCHES + 7) / 8];
boolean RemoteSwitchTestFirstPass = false;


void StopTest() {
  if (RemoteTest == REMOTE_TEST_NONE) return;
  RPU_TurnOffAllLamps();
  for (byte count = 0; count < RPU_NUMBER_OF_PLAYER_DISPLAYS; count++) RPU_SetDisplayBlank(count, 0x00);
  Audio.StopAllAudio();
  RemoteTest = REMOTE_TEST_NONE;
}

// lamp: the lamp's own number (0-59, as in StarTrek2022.h and the manifest's lamps list),
// or 60 for all lamps flashing
void StartLampTest(long lamp) {
  RPU_TurnOffAllLamps();
  if (lamp < 0 || lamp >= RPU_MAX_LAMPS) {
    for (int count = 0; count < RPU_MAX_LAMPS; count++) RPU_SetLampState(count, 1, 0, 500);
    lamp = RPU_MAX_LAMPS;
  } else {
    RPU_SetLampState(lamp, 1);
  }
  RemoteTest = ACT_LAMP_TEST;
  RemoteTestValue = lamp;
}

// digit: 0 = every digit counting 0-9, 1-6 (or 7) = only that digit
void StartDisplayTest(long digit) {
  if (digit < 0 || digit > RPU_OS_NUM_DIGITS) digit = 0;
  RemoteTest = ACT_DISPLAY_TEST;
  RemoteTestValue = digit;
}

// solenoid: the operator menu's solenoid test number; 0 = each one in turn.
// It fires every two seconds until the test is stopped.
void StartSolenoidTest(long solenoid) {
  if (solenoid < 0 || solenoid > 19) solenoid = 0;
  RemoteTest = ACT_SOLENOID_TEST;
  RemoteTestValue = solenoid;
  RemoteTestLastStep = 0;
}

void StartSwitchTest() {
  for (byte count = 0; count < sizeof(RemoteSwitchStates); count++) RemoteSwitchStates[count] = 0;
  RemoteTest = ACT_SWITCH_TEST;
  RemoteTestValue = 0;      // closures seen since the test started
  RemoteSwitchTestFirstPass = true;   // switches already closed are reported, but not counted
}

// track: the WAV Trigger track number, played once
void PlaySoundTest(long track) {
  Audio.StopAllAudio();
  if (track > 0) Audio.PlaySound((unsigned short)track, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
  RemoteTest = ACT_SOUND_TEST;
  RemoteTestValue = track;
}

void EjectAllBalls() {
  if (CountBallsInTrough()) RPU_PushToTimedSolenoidStack(SOL_SERVE_BALL, BallServeSolenoidStrength, CurrentTime + 100, true);
  if (RPU_ReadSingleSwitchState(SW_SAUCER)) RPU_PushToTimedSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, CurrentTime + 600, true);
}

// The values go to the node first (an audit snapshot), so what they were is on record.
void ClearAudits() {
  PinNodeCtrl_SendAuditsNow();
  RPU_WriteULToEEProm(RPU_TOTAL_PLAYS_EEPROM_START_BYTE, 0);
  RPU_WriteULToEEProm(RPU_TOTAL_REPLAYS_EEPROM_START_BYTE, 0);
  RPU_WriteULToEEProm(RPU_TOTAL_HISCORE_BEATEN_START_BYTE, 0);
  RPU_WriteULToEEProm(RPU_CHUTE_1_COINS_START_BYTE, 0);
  RPU_WriteULToEEProm(RPU_CHUTE_2_COINS_START_BYTE, 0);
  RPU_WriteULToEEProm(RPU_CHUTE_3_COINS_START_BYTE, 0);
  PinNodeCtrl_AuditsChanged();
}

// The credits go to zero; the count before goes to the node first, so it is on record.
void ClearCredits() {
  PinNodeCtrl_Credits(Credits, FreePlayMode);
  Credits = 0;
  RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
  // (in diagnostics the credit display belongs to the operator menu)
  if (MachineState == MACHINE_STATE_ATTRACT) RPU_SetDisplayCredits(Credits, !FreePlayMode);
}

// The adjustments go back to their defaults. Credits, the high score and the audits stay.
void RestoreDefaults() {
  byte savedCredits = Credits;
  unsigned long savedHighScore = HighScore;
  unsigned long savedAudits[6];
  for (byte count = 0; count < 6; count++) savedAudits[count] = RPU_ReadULFromEEProm(PinNodeAuditsTable[count].eeprom);

  SetAllParameterDefaults();
  Credits = savedCredits;
  HighScore = savedHighScore;
  WriteParameters(false);
  for (byte count = 0; count < 6; count++) RPU_WriteULToEEProm(PinNodeAuditsTable[count].eeprom, savedAudits[count]);

  CPCSelectionsHaveBeenRead = false;
  ReadStoredParameters();
  RPU_SetDisplayCredits(Credits, !FreePlayMode);
  PinNodeCtrl_SettingsChanged();
}

// rulesLevel: GAME_RULES_EASY, MEDIUM, HARD or PROGRESSIVE
void InstallPreset(byte rulesLevel) {
  GameRulesSelection = rulesLevel;
  RPU_WriteByteToEEProm(EEPROM_GAME_RULES_SELECTION, GameRulesSelection);
  if (LoadRuleDefaults(rulesLevel)) WriteParameters();
  PinNodeCtrl_SettingsChanged();
}


// Runs the test in progress. Called on every pass of loop() while in diagnostics.
void UpdateRemoteTest() {
  if (RemoteTest == ACT_DISPLAY_TEST) {
    RPU_CycleAllDisplays(CurrentTime, (byte)RemoteTestValue);
  } else if (RemoteTest == ACT_SOLENOID_TEST) {
    if (RemoteTestLastStep == 0 || (CurrentTime - RemoteTestLastStep) >= SOLENOID_TEST_PERIOD_MS) {
      RemoteTestLastStep = CurrentTime;
      byte solenoidNumber = (byte)RemoteTestArg;
      if (RemoteTestArg == 0) {
        // each solenoid the menu knows, in turn
        for (byte tries = 0; tries < 20; tries++) {
          RemoteTestValue = (RemoteTestValue % 19) + 1;
          if (SolenoidConvertDisplayNumberToIndex((byte)RemoteTestValue) < 0x00FD) break;
        }
        solenoidNumber = (byte)RemoteTestValue;
        // tell the app which coil this is (a repeated "ok" updates its readout)
        PinNodeCtrl_ActionResult(RemoteTestReqId, ACT_SOLENOID_TEST, PINNODE_ACTION_OK, RemoteTestValue);
      }
      unsigned short solenoidIndex = SolenoidConvertDisplayNumberToIndex(solenoidNumber);
      // (continuous solenoids, in the upper byte, are left to the coin-door menu)
      if (solenoidIndex < 0x00FD) {
        byte strength = SolenoidConvertDisplayNumberToTestStrength(solenoidNumber);
        if (strength >= OPERATOR_MENU_VALUE_UNUSED) strength = 4;
        RPU_PushToSolenoidStack((byte)solenoidIndex, strength, true);
      }
    }
  } else if (RemoteTest == ACT_SWITCH_TEST) {
    for (byte sw = 0; sw < SWITCH_TEST_NUM_SWITCHES; sw++) {
      boolean closed = RPU_ReadSingleSwitchState(sw);
      boolean wasClosed = (RemoteSwitchStates[sw / 8] >> (sw % 8)) & 0x01;
      if (closed != wasClosed) {
        if (closed) {
          RemoteSwitchStates[sw / 8] |= (1 << (sw % 8));
          if (!RemoteSwitchTestFirstPass) RemoteTestValue += 1;
        } else {
          RemoteSwitchStates[sw / 8] &= ~(1 << (sw % 8));
        }
        PinNodeCtrl_SwitchEvent(sw, closed);
      }
    }
    RemoteSwitchTestFirstPass = false;
  }
}


// A test that was running when the machine left diagnostics is reported as stopped
void EndDiagnostics() {
  if (RemoteTest != REMOTE_TEST_NONE) {
    byte runningTest = RemoteTest;
    long value = RemoteTestValue;
    StopTest();
    PinNodeCtrl_ActionResult(RemoteTestReqId, runningTest, PINNODE_ACTION_DONE, value, PINNODE_REASON_DIAGNOSTICS);
  }
  RPU_TurnOffAllLamps();
  Audio.StopAllAudio();
}


// Takes a request from the app, where the game can act on it: attract and diagnostics.
// (Anywhere else nobody takes it, and the library answers "not now".)
int HandleOperatorAction(int curState) {
  int returnState = curState;
  boolean inDiagnostics = (curState == MACHINE_STATE_DIAGNOSTICS);

  PinNodeActionRequest request;
  if (!PinNodeCtrl_TakeAction(&request)) return returnState;

  if (request.op == PINNODE_OP_STOP) {
    long value = 0;
    if (RemoteTest == request.action) {
      value = RemoteTestValue;
      StopTest();
    }
    PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_DONE, value, request.reason);
    return returnState;
  }

  if (request.action <= ACT_SOUND_TEST) {
    // a test
    if (!inDiagnostics) {
      PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_NOT_NOW, 0);
      return returnState;
    }
    if (RemoteTest != REMOTE_TEST_NONE && RemoteTest != request.action) StopTest();
    RemoteTestReqId = request.reqId;
    RemoteTestArg = request.arg;
    switch (request.action) {
      case ACT_LAMP_TEST: StartLampTest(request.arg); break;
      case ACT_DISPLAY_TEST: StartDisplayTest(request.arg); break;
      case ACT_SOLENOID_TEST: StartSolenoidTest(request.arg); break;
      case ACT_SWITCH_TEST: StartSwitchTest(); break;
      case ACT_SOUND_TEST: PlaySoundTest(request.arg); break;
    }
    PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_OK, RemoteTestValue);
    return returnState;
  }

  // a one-shot (the two clears answer with what was there before: total plays, credits)
  long previousValue = 0;
  switch (request.action) {
    case ACT_EJECT_BALLS: EjectAllBalls(); break;
    case ACT_CLEAR_AUDITS:
      previousValue = (long)RPU_ReadULFromEEProm(RPU_TOTAL_PLAYS_EEPROM_START_BYTE);
      ClearAudits();
      break;
    case ACT_CLEAR_CREDITS:
      previousValue = Credits;
      ClearCredits();
      break;
    case ACT_FACTORY_DEFAULTS: RestoreDefaults(); break;
    case ACT_PRESET_EASY: InstallPreset(GAME_RULES_EASY); break;
    case ACT_PRESET_MEDIUM: InstallPreset(GAME_RULES_MEDIUM); break;
    case ACT_PRESET_HARD: InstallPreset(GAME_RULES_HARD); break;
    case ACT_PRESET_PROGRESSIVE: InstallPreset(GAME_RULES_PROGRESSIVE); break;
    case ACT_ENTER_DIAGNOSTICS: returnState = MACHINE_STATE_DIAGNOSTICS; break;
    case ACT_EXIT_DIAGNOSTICS:
      if (inDiagnostics) {
        EndDiagnostics();
        returnState = MACHINE_STATE_ATTRACT;
      }
      break;
    default:
      PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_UNKNOWN, 0);
      return returnState;
  }
  PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_DONE, previousValue, PINNODE_REASON_FINISHED);
  return returnState;
}


// The tournament lock: while it's on, a difficulty setting can't be changed at the coin
// door either. The menu has already saved the edit by the time the game hears of it, so
// the old value is remembered when the adjustment is opened and put back.
byte *GuardedAdjustmentByte = NULL;
unsigned long *GuardedAdjustmentUL = NULL;
unsigned short GuardedAdjustmentEeprom = 0;
unsigned long GuardedAdjustmentValue = 0;
boolean AdjustmentGuarded = false;

void GuardAdjustmentIfLocked(unsigned short eepromAddress, byte *adjustmentByte, unsigned long *adjustmentUL) {
  AdjustmentGuarded = false;
  if (eepromAddress == 0 || (adjustmentByte == NULL && adjustmentUL == NULL)) return;
  for (byte count = 0; count < (sizeof(PinNodeSettingsTable) / sizeof(PinNodeSettingsTable[0])); count++) {
    if (PinNodeSettingsTable[count].eeprom == eepromAddress && PinNodeCtrl_SettingLocked(count)) {
      AdjustmentGuarded = true;
      GuardedAdjustmentEeprom = eepromAddress;
      GuardedAdjustmentByte = adjustmentByte;
      GuardedAdjustmentUL = adjustmentUL;
      GuardedAdjustmentValue = adjustmentUL ? (*adjustmentUL) : (unsigned long)(*adjustmentByte);
      return;
    }
  }
}

// true if the edit was refused
boolean RevertGuardedAdjustment() {
  if (!AdjustmentGuarded) return false;
  if (GuardedAdjustmentUL) {
    *GuardedAdjustmentUL = GuardedAdjustmentValue;
    RPU_WriteULToEEProm(GuardedAdjustmentEeprom, GuardedAdjustmentValue);
  } else {
    *GuardedAdjustmentByte = (byte)GuardedAdjustmentValue;
    RPU_WriteByteToEEProm(GuardedAdjustmentEeprom, (byte)GuardedAdjustmentValue);
  }
  return true;
}
#endif


////////////////////////////////////////////////////////////////////////////
//
//  Achievements and stats
//    An achievement is awarded once per player per game; the indices are
//    the ACH_ and STAT_ constants of PinNodeGame.h (from pinnode.game.json).
//
////////////////////////////////////////////////////////////////////////////
void AwardAchievement(byte achievementIndex) {
  if (achievementIndex > 15) return;
  unsigned short achievementBit = ((unsigned short)1) << achievementIndex;
  if (AchievementsAwarded[CurrentPlayer] & achievementBit) return;
  AchievementsAwarded[CurrentPlayer] |= achievementBit;
#ifdef RPU_OS_USE_PINNODE_LINK
  PinNodeCtrl_Achievement(CurrentPlayer + 1, achievementIndex);
#endif
}

void AddToStat(byte statIndex, long amount = 1) {
#ifdef RPU_OS_USE_PINNODE_LINK
  PinNodeCtrl_Stat(CurrentPlayer + 1, statIndex, PINNODE_STAT_ADD, amount);
#else
  (void)statIndex;
  (void)amount;
#endif
}

void RecordStatMax(byte statIndex, long value) {
#ifdef RPU_OS_USE_PINNODE_LINK
  PinNodeCtrl_Stat(CurrentPlayer + 1, statIndex, PINNODE_STAT_MAX, value);
#else
  (void)statIndex;
  (void)value;
#endif
}


////////////////////////////////////////////////////////////////////////////
//
//  Setup
//    Arduino calls this function at power up and reset.
//    It's used to initialize the hardware and 
//    certain variables and structures used by 
//    the code.
//
////////////////////////////////////////////////////////////////////////////

#ifdef RPU_OS_USE_PINNODE_LINK
// Start the link and say HELLO (PinNodeCtrl_Update repeats it whenever the link comes back).
// The tables are registered first, so HELLO carries the settings count.
void StartPinNodeLink() {
  PinNodeCtrl_Settings(PinNodeSettingsTable, sizeof(PinNodeSettingsTable) / sizeof(PinNodeSettingsTable[0]), PinNodeSettingsApplied);
  PinNodeCtrl_Audits(PinNodeAuditsTable, sizeof(PinNodeAuditsTable) / sizeof(PinNodeAuditsTable[0]));
  PinNodeCtrl_Actions(PinNodeActionsTable, sizeof(PinNodeActionsTable) / sizeof(PinNodeActionsTable[0]));
  RPULink_BeginUart();   // the ESP32 in the WiFi socket, on USART2
  byte PinNodeManifest[8];
  PinNodeManifestBytes(PinNodeManifest);
  PinNodeCtrl_Begin(PINNODE_RULESET_VERSION, PINNODE_RULESET_ID, PinNodeManifest, RPU_NUMBER_OF_PLAYERS_ALLOWED, 0, RPU_OS_HARDWARE_REV);
}
#endif

// What this sketch still has to do while the original ROM game runs
void OriginalGameIdle() {
#ifdef RPU_OS_USE_ACCESSORY_LAMP_BOARD
  // Send a periodic message to keep GI on
  if (GIInOriginal) {
    if (millis() >  (ALBWatchdogResetLastSent + ALB_WATCHDOG_TIMER_DURATION/2)) {
      TopperALB.WatchdogTimerReset();
      ALBWatchdogResetLastSent = millis();
    }
  }
#endif
}

void setup() {

  if (DEBUG_MESSAGES) {
    // If debug is on, set up the Serial port for communication
    Serial.begin(115200);
    Serial.write("Starting\n");
  }

  // Set up the Audio handler in order to play boot messages
  CurrentTime = millis();
  Audio.InitDevices(AUDIO_PLAY_TYPE_WAV_TRIGGER | AUDIO_PLAY_TYPE_ORIGINAL_SOUNDS);
  Audio.StopAllAudio();
  // As the 2022 code did it: a callout turns the music down 16 dB (from -4 to -20 then),
  // and leaves the sound effects alone. With effects ducked too, anything started during
  // a callout stayed 20 dB down for its whole length: the 20-second new game theme, which
  // starts under the "player one" callout, could hardly be heard.
  Audio.SetMusicDuckingGain(16);
  Audio.SetSoundFXDuckingGain(0);

  // Tell the OS about game-specific switches
  // (this is for software-controlled pop bumpers and slings)
#if (RPU_MPU_ARCHITECTURE<10)
  // Machines with a -17, -35, 100, and 200 architecture
  // almost always have software based switch-triggered solenoids.
  // For those, you can define an array of solenoids and the switches
  // that will trigger them:
  RPU_SetupGameSwitches(NUM_SWITCHES_WITH_TRIGGERS, NUM_PRIORITY_SWITCHES_WITH_TRIGGERS, SolenoidAssociatedSwitches);

#endif

  // Set up the chips and interrupts
  unsigned long initResult = 0;
  if (DEBUG_MESSAGES) Serial.write("Initializing MPU\n");

  // If the hardware has the ability to switch on the Credit/Reset button (requires Rev 4 or greater)
  // then that can be used to choose Original or New code. Otherwise, the hardware switch
  // will choose Original if open, and New if closed
  unsigned long initOptions = RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET | RPU_CMD_BOOT_ORIGINAL_IF_NOT_SWITCH_CLOSED |
                              RPU_CMD_INIT_AND_RETURN_EVEN_IF_ORIGINAL_CHOSEN | RPU_CMD_PERFORM_MPU_TEST;
#ifdef RPU_OS_USE_PINNODE_LINK
  // The owner can choose the original ROM game from the app or the web. With that choice
  // saved, the machine boots the original unless Credit/Reset is held at power-on (which
  // boots this game and clears the choice).
  initOptions = PinNode_BootOptions(initOptions);
#endif
  initResult = RPU_InitializeMPU(initOptions, SW_CREDIT_RESET);
#ifdef RPU_OS_USE_PINNODE_LINK
  PinNode_BootChosen(initResult);
#endif

  if (DEBUG_MESSAGES) {
    char buf[128];
    sprintf(buf, "Return from init = 0x%04lX\n", initResult);
    Serial.write(buf);
    if (initResult & RPU_RET_6800_DETECTED) Serial.write("Detected 6800 clock\n");
    else if (initResult & RPU_RET_6802_OR_8_DETECTED) Serial.write("Detected 6802/8 clock\n");
    Serial.write("Back from init\n");
  }

  if (initResult & RPU_RET_SELECTOR_SWITCH_ON) QueueDIAGNotification(SOUND_EFFECT_DIAG_SELECTOR_SWITCH_ON);
  else QueueDIAGNotification(SOUND_EFFECT_DIAG_SELECTOR_SWITCH_OFF);

  if (initResult & RPU_RET_CREDIT_RESET_BUTTON_HIT) QueueDIAGNotification(SOUND_EFFECT_DIAG_CREDIT_RESET_BUTTON);

  if (initResult & RPU_RET_DIAGNOSTIC_REQUESTED) {
    QueueDIAGNotification(SOUND_EFFECT_DIAG_STARTING_DIAGNOSTICS);
    // Run diagnostics here:
  }

  if (initResult & RPU_RET_ORIGINAL_CODE_REQUESTED) {
    if (DEBUG_MESSAGES) Serial.write("Asked to run original code\n");
    delay(100);
    QueueDIAGNotification(SOUND_EFFECT_DIAG_STARTING_ORIGINAL_CODE);
    delay(100);

#ifdef RPU_OS_USE_ACCESSORY_LAMP_BOARD
    TopperALB.InitOutogingCommunication();
    TopperALB.SetTargetDeviceAddress(8);
    TopperALB.EnableLamps();
    TopperALB.WatchdogTimerReset();
    TopperALB.AdjustSetting(SET_COLOR_0, 0, 0, 200);
    TopperALB.AdjustSetting(SET_COLOR_1, 200, 200, 0);
    ALBWatchdogResetLastSent = millis();
#endif

    while (Audio.Update(millis()));
#ifdef RPU_OS_USE_PINNODE_LINK
    // The original game has the machine. This keeps the link to the node up (it reports
    // "original"), so the owner can switch back from the app; it never returns.
    StartPinNodeLink();
    PinNode_RunOriginal(OriginalGameIdle);
#else
    // Arduino should hang if original code is running
    while (1) {
      OriginalGameIdle();
    }
#endif
  }
  QueueDIAGNotification(SOUND_EFFECT_DIAG_STARTING_NEW_CODE);

  RPU_DisableSolenoidStack();
  RPU_SetDisableFlippers(true);

  // Read parameters from EEProm
  ReadStoredParameters();

#ifdef RPU_OS_USE_PINNODE_LINK
  StartPinNodeLink();
#endif

  CurrentScores[0] = GAME_MAJOR_VERSION;
  CurrentScores[1] = GAME_MINOR_VERSION;
  CurrentScores[2] = RPU_OS_MAJOR_VERSION;
  CurrentScores[3] = RPU_OS_MINOR_VERSION;

  CurrentAchievements[0] = 0;
  CurrentAchievements[1] = 0;
  CurrentAchievements[2] = 0;
  CurrentAchievements[3] = 0;

  DropTargets.DefineSwitch(0, SW_DROP_1);
  DropTargets.DefineSwitch(1, SW_DROP_2);
  DropTargets.DefineSwitch(2, SW_DROP_3);
  DropTargets.DefineSwitch(3, SW_DROP_4);
  DropTargets.DefineResetSolenoid(0, SOL_DROP_TARGET_RESET);

  CurrentTime = millis();

  // Clear the playfield at power-on: raise the drop targets and empty the saucer
  RPU_PushToTimedSolenoidStack(SOL_DROP_TARGET_RESET, DROP_TARGET_RESET_STRENGTH, CurrentTime + 100, true);
  RPU_PushToTimedSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, CurrentTime + 600, true);

  Audio.QueueSound(SOUND_EFFECT_MACHINE_START, AUDIO_PLAY_TYPE_WAV_TRIGGER, CurrentTime + 1200);
  OperatorSwitchPressStarted = 0;
  InOperatorMenu = false;

  // CUSTOMIZATION TODO:
  // The operator menus can use different configurations for navigation buttons.
  // I've left a commented out examples from Black Knight and Firepower to show how forward/back buttons
  // can be configured if you have them
  Menus.SetNavigationButtons(SWITCH_STACK_EMPTY, SWITCH_STACK_EMPTY, SW_CREDIT_RESET, SW_SELF_TEST_SWITCH);
  // Menus.SetNavigationButtons(SW_RIGHT_MAGNET_BUTTON, SW_LEFT_MAGNET_BUTTON, SW_CREDIT_RESET, SW_SELF_TEST_SWITCH);
  // Menus.SetNavigationButtons(SW_RIGHT_FLIPPER, SW_HIGH_SCORE_RESET, SW_CREDIT_RESET, SW_SELF_TEST_SWITCH);

  Menus.SetLampsLookupCallback(LampConvertDisplayNumberToIndex);
  Menus.SetSolenoidIDLookupCallback(SolenoidConvertDisplayNumberToIndex);
  Menus.SetSolenoidStrengthLookupCallback(SolenoidConvertDisplayNumberToTestStrength);
  Menus.SetMenuButtonDebounce(250);

#ifdef RPU_OS_USE_ACCESSORY_LAMP_BOARD
  TopperALB.InitOutogingCommunication();
  TopperALB.SetTargetDeviceAddress(8);
  TopperALB.EnableLamps();
  TopperALB.WatchdogTimerReset();
  TopperALB.AdjustSetting(SET_COLOR_0, 0, 0, 200);
  TopperALB.AdjustSetting(SET_COLOR_1, 200, 200, 0);
  ALBWatchdogResetLastSent = millis();
#endif

  LastTiltWarningTime = 0;
}

byte ReadSetting(byte setting, byte defaultValue, byte maxValue) {
  byte value = EEPROM.read(setting);
  if (value == 0xFF || value>maxValue) {
    EEPROM.write(setting, defaultValue);
    return defaultValue;
  }
  return value;
}

// This function is useful for checking the status of drop target switches
byte CheckSequentialSwitches(byte startingSwitch, byte numSwitches) {
  byte returnSwitches = 0;
  for (byte count = 0; count < numSwitches; count++) {
    returnSwitches |= (RPU_ReadSingleSwitchState(startingSwitch + count) << count);
  }
  return returnSwitches;
}

// CUSTOMIZATION TODO:
// Set up lamp functions so the lamps will reflect
// the current game state. For organization, create a different
// function for groups of similar lamps, like ShowTopLanes()
////////////////////////////////////////////////////////////////////////////
//
//  Lamp Management functions
//    These functions are called each time through the gameplay loop.
//    They use the current status variables to set each lamp to 
//    on, off, dim (depending on hardware), or flashing. The lamps are
//    actually set in hardware by the Interrupt Service Routine, so
//    these functions simply update the state arrays used by the ISR.
//
//    If a special animation is required (sweeps and such), then these
//    functions are turned off and one of the special functions in
//    "LampAnimations.h" is used to set those lamps.
//
////////////////////////////////////////////////////////////////////////////
void SetGeneralIlluminationOn(boolean setGIOn = true) {
  // Since this machine doesn't have GI control,
  // this line prevents compiler warnings.
  (void)setGIOn;
}

void ShowPlayerLamps() {
  // As in the 2022 code: only the player who is up
  for (byte count = 0; count < 4; count++) {
    RPU_SetLampState(PlayerUpLamps[count], count==CurrentPlayer);
  }
}

// The bonus lamps add up: 20K, 10K, and 1K-9K lit up to the units (35 = 20K + 10K + 1K..5K)

void ShowBonusLamps() {
  if (GameMode == GAME_MODE_SKILL_SHOT) {
    byte bonusPhase = ((CurrentTime - GameModeStartTime) / 100) % 12;
    for (byte count = 0; count < 11; count++) {
      RPU_SetLampState(LAMP_BONUS_1K + count, (bonusPhase % 11) == count);
    }
  } else {
    byte curBonus = Bonus[CurrentPlayer];
    for (byte count = 0; count < 9; count++) {
      RPU_SetLampState(LAMP_BONUS_1K + count, (curBonus % 10) > count);
    }
    RPU_SetLampState(LAMP_BONUS_10K, (curBonus % 20) >= 10);
    RPU_SetLampState(LAMP_BONUS_20K, curBonus >= 20);
  }
}


void ShowBonusXLamps() {
  if (GameMode == GAME_MODE_SKILL_SHOT) {
    byte lampPhase = ((CurrentTime - GameModeStartTime) / 200) % 3;
    RPU_SetLampState(LAMP_BONUS_2X, lampPhase == 0);
    RPU_SetLampState(LAMP_BONUS_3X, lampPhase == 0);
    RPU_SetLampState(LAMP_SPECIAL, lampPhase == 1);
    RPU_SetLampState(LAMP_PLANET_2X, lampPhase == 1);
  } else {
    RPU_SetLampState(LAMP_BONUS_2X, BonusX[CurrentPlayer] == 2);
    RPU_SetLampState(LAMP_BONUS_3X, BonusX[CurrentPlayer] == 3);
    // (LAMP_SPECIAL is the top of the drop target ladder: ShowDTBonusLamps sets it)
  }
}


// Drop target ladder: 10K / 25K / 50K / SPECIAL, and the planet 2X lamp
void ShowDTBonusLamps() {
  if (GameMode != GAME_MODE_SKILL_SHOT) {
    for (byte count = 0; count < 4; count++) {
      RPU_SetLampState(LAMP_SPECIAL_10K + count, count < DTBonusIDX);
    }
    RPU_SetLampState(LAMP_PLANET_2X, DTBonusMultiplier == 2);
  }
}


byte LetterToByteConversion[5] = {16, 8, 4, 2, 1};
byte LettertoLamp[5] = {LAMP_B, LAMP_A, LAMP_L, LAMP_2L, LAMP_Y};
byte LettertoSwitchLamp[5] = {LAMP_ROLLOVER_B, LAMP_ROLLOVER_A, LAMP_TARGET_L, LAMP_TARGET_2L, LAMP_TARGET_Y};

void ShowBallyLamps() {
  boolean spelledAnimation = (BallyAnimationBegin != 0 && (CurrentTime - BallyAnimationBegin) < 2500);

  // Skill shot: the top lane to plunge into blinks. (Each lamp is set once per pass: set
  // steady and then to blink, the lamp interrupt catches either and it flickers.)
  byte skillShotLamp = 0xFF;
  if (GameMode == GAME_MODE_SKILL_SHOT) skillShotLamp = SkillShotLane ? LAMP_ROLLOVER_A : LAMP_ROLLOVER_B;

  for (byte count = 0; count < 5; count++) {
    byte switchLampOn;
    if (spelledAnimation) {
      // all five letters flash after B-A-L-L-Y is spelled
      RPU_SetLampState(LettertoLamp[count], 1, 0, 150);
      switchLampOn = 1;
    } else if (BallyStatus[CurrentPlayer] & LetterToByteConversion[count]) {
      RPU_SetLampState(LettertoLamp[count], 1);
      switchLampOn = 0;
    } else {
      RPU_SetLampState(LettertoLamp[count], 0);
      switchLampOn = 1;
    }
    if (LettertoSwitchLamp[count] == skillShotLamp) RPU_SetLampState(skillShotLamp, 1, 0, SKILL_SHOT_BLINK_MS);
    else RPU_SetLampState(LettertoSwitchLamp[count], switchLampOn);
    // the letter the saucer will spot
    RPU_SetLampState(LAMP_SAUCER_B + count, count == SaucerLetterIDX);
  }
}


byte LaneLampIDX0[3] = {LAMP_R_UPPER_CHUTE_5K, LAMP_R_OUTER_LANE, LAMP_L_OUTER_LANE};
byte LaneLampIDX1[3] = {LAMP_L_UPPER_CHUTE_5K, LAMP_R_INNER_LANE, LAMP_L_INNER_LANE};

void ShowLaneLamps() {
  for (byte count = 0; count < 3; count++) {
    RPU_SetLampState(LaneLampIDX1[count], LaneLightPhase);
    RPU_SetLampState(LaneLampIDX0[count], !LaneLightPhase);
  }
}


void ShowHyperSpaceLamps() {
  if (HyperSpaceExtraBonus != 0 && (CurrentTime - HyperSpaceExtraBonus) >= 30000) {
    HyperSpaceExtraBonus = 0;   // the bonus window is over
  }

  if (GameMode == GAME_MODE_SKILL_SHOT || HyperSpaceExtraBonus != 0) {
    // a chase up the six hyperspace lamps
    if ((CurrentTime - HSLampAnimationDelay) > 100) {
      HSLampAnimationDelay = CurrentTime;
      for (byte count = 0; count < 6; count++) {
        RPU_SetLampState(LAMP_HS_2K + count, count == HSLampLit);
      }
      HSLampLit += 1;
      if (HSLampLit >= 7) HSLampLit = 0;
    }
  } else {
    for (byte count = 0; count < 6; count++) {
      RPU_SetLampState(LAMP_HS_2K + count, count < HSBonusIDX[CurrentPlayer]);
    }
  }
}



void ShowShootAgainLamp() {

  if ( (BallFirstSwitchHitTime == 0 && BallSaveNumSeconds) || (BallSaveEndTime && CurrentTime < BallSaveEndTime) ) {
    unsigned long msRemaining = 5000;
    if (BallSaveEndTime != 0) msRemaining = BallSaveEndTime - CurrentTime;
    RPU_SetLampState(LAMP_SHOOT_AGAIN, 1, 0, (msRemaining < 5000) ? 100 : 500);
  } else {
    RPU_SetLampState(LAMP_SHOOT_AGAIN, SamePlayerShootsAgain);
  }
  RPU_SetLampState(LAMP_HEAD_SHOOT_AGAIN, SamePlayerShootsAgain, 0, 500);
}





////////////////////////////////////////////////////////////////////////////
//
//  Machine State Helper functions
//
////////////////////////////////////////////////////////////////////////////
boolean AddPlayer(boolean resetNumPlayers = false) {

  if (Credits < 1 && !FreePlayMode) return false;
  if (resetNumPlayers) CurrentNumPlayers = 0;
  if (CurrentNumPlayers >= RPU_NUMBER_OF_PLAYERS_ALLOWED) return false;

  CurrentNumPlayers += 1;
  RPU_SetDisplay(CurrentNumPlayers - 1, 0, true, 2);
#ifdef RPU_OS_USE_PINNODE_LINK
  // Player 1 is reported by PinNodeCtrl_GameStart; this is players 2-4 joining
  if (CurrentNumPlayers > 1) PinNodeCtrl_PlayerAdded(CurrentNumPlayers);
#endif
  
  if (CurrentNumPlayers > 1) {
    // If this is second, third, or fourth player, then playe the announcment
    QueueNotification(SOUND_EFFECT_ADD_PLAYER_1 + (CurrentNumPlayers - 1), 1);
  }

  for (byte count = 0; count < 4; count++) {
    if (count==CurrentPlayer) RPU_SetLampState(PlayerUpLamps[count], 1, 0, 250);
    else if (count<CurrentNumPlayers) RPU_SetLampState(PlayerUpLamps[count], 1);
    else RPU_SetLampState(PlayerUpLamps[count], 0);
  }

  if (!FreePlayMode) {
    Credits -= 1;
    RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
    RPU_SetDisplayCredits(Credits, !FreePlayMode);
    RPU_SetCoinLockout(false);
  }

  RPU_WriteULToEEProm(RPU_TOTAL_PLAYS_EEPROM_START_BYTE, RPU_ReadULFromEEProm(RPU_TOTAL_PLAYS_EEPROM_START_BYTE) + 1);

  return true;
}


unsigned short ChuteAuditByte[] = {RPU_CHUTE_1_COINS_START_BYTE, RPU_CHUTE_2_COINS_START_BYTE, RPU_CHUTE_3_COINS_START_BYTE};
void AddCoinToAudit(byte chuteNum) {
  if (chuteNum > 2) return;
  unsigned short coinAuditStartByte = ChuteAuditByte[chuteNum];
  RPU_WriteULToEEProm(coinAuditStartByte, RPU_ReadULFromEEProm(coinAuditStartByte) + 1);
}


void AddCredit(boolean playSound = false, byte numToAdd = 1) {
  if (Credits < MaximumCredits) {
    Credits += numToAdd;
    if (Credits > MaximumCredits) Credits = MaximumCredits;
    RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
    if (playSound) {
      //PlaySoundEffect(SOUND_EFFECT_ADD_CREDIT);
      RPU_PushToSolenoidStack(SOL_KNOCKER, KNOCKER_SOLENOID_STRENGTH, true);
    }
    RPU_SetDisplayCredits(Credits, !FreePlayMode);
    RPU_SetCoinLockout(false);
  } else {
    RPU_SetDisplayCredits(Credits, !FreePlayMode);
    RPU_SetCoinLockout(true);
  }

}

byte SwitchToChuteNum(byte switchHit) {
  byte chuteNum = 0;
  if (switchHit == SW_COIN_2) chuteNum = 1;
  else if (switchHit == SW_COIN_3) chuteNum = 2;
  return chuteNum;
}

boolean AddCoin(byte chuteNum) {
  boolean creditAdded = false;
  if (chuteNum > 2) return false;
  byte cpcSelection = GetCPCSelection(chuteNum);

  // Find the lowest chute num with the same ratio selection
  // and use that ChuteCoinsInProgress counter
  byte chuteNumToUse;
  for (chuteNumToUse = 0; chuteNumToUse <= chuteNum; chuteNumToUse++) {
    if (GetCPCSelection(chuteNumToUse) == cpcSelection) break;
  }

  PlaySoundEffect(SOUND_EFFECT_ADD_CREDIT_1 + (CurrentTime % 3));

  byte cpcCoins = GetCPCCoins(cpcSelection);
  byte cpcCredits = GetCPCCredits(cpcSelection);
  byte coinProgressBefore = ChuteCoinsInProgress[chuteNumToUse];
  ChuteCoinsInProgress[chuteNumToUse] += 1;

  if (ChuteCoinsInProgress[chuteNumToUse] == cpcCoins) {
    if (cpcCredits > cpcCoins) AddCredit(cpcCredits - (coinProgressBefore));
    else AddCredit(cpcCredits);
    ChuteCoinsInProgress[chuteNumToUse] = 0;
    creditAdded = true;
  } else {
    if (cpcCredits > cpcCoins) {
      AddCredit(1);
      creditAdded = true;
    } else {
    }
  }

  return creditAdded;
}


void AddSpecialCredit() {
  AddCredit(false, 1);
  RPU_PushToTimedSolenoidStack(SOL_KNOCKER, KNOCKER_SOLENOID_STRENGTH, CurrentTime, true);
  RPU_WriteULToEEProm(RPU_TOTAL_REPLAYS_EEPROM_START_BYTE, RPU_ReadULFromEEProm(RPU_TOTAL_REPLAYS_EEPROM_START_BYTE) + 1);
}

void AwardSpecial() {
  if (SpecialCollected) return;
  SpecialCollected = true;
  if (TournamentScoring) {
    CurrentScores[CurrentPlayer] += SpecialValue * PlayfieldMultiplier;
  } else {
    AddSpecialCredit();
  }
}

// One extra ball per ball in play, as in the 2022 code
boolean AwardExtraBall() {
  if (ExtraBallCollected) return false;
  ExtraBallCollected = true;
  if (TournamentScoring) {
    CurrentScores[CurrentPlayer] += ExtraBallValue * PlayfieldMultiplier;
    PlaySoundEffect(SOUND_EFFECT_SKILL_SHOT);
  } else {
    SamePlayerShootsAgain = true;
    RPU_SetLampState(LAMP_SHOOT_AGAIN, SamePlayerShootsAgain);
    RPU_SetLampState(LAMP_HEAD_SHOOT_AGAIN, SamePlayerShootsAgain);
    PlaySoundEffect(SOUND_EFFECT_EXTRA_BALL);
  }
  return true;
}


void IncreasePlayfieldMultiplier(unsigned long duration) {
  PlayfieldMultiplierTimeLeft += duration;

  PlayfieldMultiplier += 1;
  if (PlayfieldMultiplier > 5) {
    PlayfieldMultiplier = 5;
  }
}


void SetBallSave(unsigned long duration, byte numberOfSaves = 0xFF, boolean addToBallSave = false) {

  if (duration == 0) {
    BallSaveEndTime = 0;
    NumberOfBallSavesRemaining = 0;
  } else if (addToBallSave) {
    if (BallSaveEndTime) BallSaveEndTime += duration;
  } else {
    BallSaveEndTime = CurrentTime + duration;
    NumberOfBallSavesRemaining = numberOfSaves;
  }
}




#define SOUND_EFFECT_OM_CPC_VALUES                  180
#define SOUND_EFFECT_OM_CRB_VALUES                  210
#define SOUND_EFFECT_OM_DIFFICULTY_VALUES           220

#define SOUND_EFFECT_AP_TOP_LEVEL_MENU_ENTRY    1700
#define SOUND_EFFECT_AP_TEST_MENU               1701
#define SOUND_EFFECT_AP_AUDITS_MENU             1702
#define SOUND_EFFECT_AP_BASIC_ADJUSTMENTS_MENU  1703
#define SOUND_EFFECT_AP_GAME_RULES_LEVEL        1704
#define SOUND_EFFECT_AP_GAME_SPECIFIC_ADJ_MENU  1705

#define SOUND_EFFECT_AP_TEST_LAMPS              1710
#define SOUND_EFFECT_AP_TEST_DISPLAYS           1711
#define SOUND_EFFECT_AP_TEST_SOLENOIDS          1712
#define SOUND_EFFECT_AP_TEST_SWITCHES           1713
#define SOUND_EFFECT_AP_TEST_SOUNDS             1714
#define SOUND_EFFECT_AP_TEST_EJECT_BALLS        1715

#define SOUND_EFFECT_AP_AUDIT_TOTAL_PLAYS       1720
#define SOUND_EFFECT_AP_AUDIT_CHUTE_1_COINS     1721
#define SOUND_EFFECT_AP_AUDIT_CHUTE_2_COINS     1722
#define SOUND_EFFECT_AP_AUDIT_CHUTE_3_COINS     1723
#define SOUND_EFFECT_AP_AUDIT_TOTAL_REPLAYS     1724
#define SOUND_EFFECT_AP_AUDIT_AVG_BALL_TIME     1725
#define SOUND_EFFECT_AP_AUDIT_HISCR_BEAT        1726
#define SOUND_EFFECT_AP_AUDIT_TOTAL_BALLS       1727
#define SOUND_EFFECT_AP_AUDIT_NUM_MATCHES       1728
#define SOUND_EFFECT_AP_AUDIT_MATCH_PERCENTAGE  1729
#define SOUND_EFFECT_AP_AUDIT_LIFETIME_PLAYS    1730
#define SOUND_EFFECT_AP_AUDIT_MINUTES_ON        1731
#define SOUND_EFFECT_AP_AUDIT_CLEAR_AUDITS      1732

#define OM_BASIC_ADJ_IDS_FREEPLAY               0
#define OM_BASIC_ADJ_IDS_BALL_SAVE              1
#define OM_BASIC_ADJ_IDS_TILT_WARNINGS          2
#define OM_BASIC_ADJ_IDS_MUSIC_VOLUME           3
#define OM_BASIC_ADJ_IDS_SOUNDFX_VOLUME         4
#define OM_BASIC_ADJ_IDS_CALLOUTS_VOLUME        5
#define OM_BASIC_ADJ_IDS_BALLS_PER_GAME         6
#define OM_BASIC_ADJ_IDS_TOURNAMENT_MODE        7
#define OM_BASIC_ADJ_IDS_EXTRA_BALL_VALUE       8
#define OM_BASIC_ADJ_IDS_SPECIAL_VALUE          9 
#define OM_BASIC_ADJ_IDS_RESET_DURING_GAME      10
#define OM_BASIC_ADJ_IDS_SCORE_LEVEL_1          11
#define OM_BASIC_ADJ_IDS_SCORE_LEVEL_2          12
#define OM_BASIC_ADJ_IDS_SCORE_LEVEL_3          13
#define OM_BASIC_ADJ_IDS_SCORE_AWARDS           14
#define OM_BASIC_ADJ_IDS_SCROLLING_SCORES       15
#define OM_BASIC_ADJ_IDS_HISCR                  16
#define OM_BASIC_ADJ_IDS_CREDITS                17
#define OM_BASIC_ADJ_IDS_CPC_1                  18
#define OM_BASIC_ADJ_IDS_CPC_2                  19
#define OM_BASIC_ADJ_IDS_CPC_3                  20
#define OM_BASIC_ADJ_IDS_MATCH_FEATURE          21
#define OM_BASIC_ADJ_FINISHED                   22
#define SOUND_EFFECT_AP_FREEPLAY                (1740 + OM_BASIC_ADJ_IDS_FREEPLAY)
#define SOUND_EFFECT_AP_BALL_SAVE_SECONDS       (1740 + OM_BASIC_ADJ_IDS_BALL_SAVE)
#define SOUND_EFFECT_AP_TILT_WARNINGS           (1740 + OM_BASIC_ADJ_IDS_TILT_WARNINGS)
#define SOUND_EFFECT_AP_MUSIC_VOLUME            (1740 + OM_BASIC_ADJ_IDS_MUSIC_VOLUME)
#define SOUND_EFFECT_AP_SOUNDFX_VOLUME          (1740 + OM_BASIC_ADJ_IDS_SOUNDFX_VOLUME)
#define SOUND_EFFECT_AP_CALLOUTS_VOLUME         (1740 + OM_BASIC_ADJ_IDS_CALLOUTS_VOLUME)
#define SOUND_EFFECT_AP_BALLS_PER_GAME          (1740 + OM_BASIC_ADJ_IDS_BALLS_PER_GAME)
#define SOUND_EFFECT_AP_TOURNAMENT_MODE         (1740 + OM_BASIC_ADJ_IDS_TOURNAMENT_MODE)
#define SOUND_EFFECT_AP_EXTRA_BALL_VALUE        (1740 + OM_BASIC_ADJ_IDS_EXTRA_BALL_VALUE)
#define SOUND_EFFECT_AP_SPECIAL_VALUE           (1740 + OM_BASIC_ADJ_IDS_SPECIAL_VALUE)
#define SOUND_EFFECT_AP_RESET_DURING_GAME       (1740 + OM_BASIC_ADJ_IDS_RESET_DURING_GAME)
#define SOUND_EFFECT_AP_ADJ_SCORE_LEVEL_1       (1740 + OM_BASIC_ADJ_IDS_SCORE_LEVEL_1)
#define SOUND_EFFECT_AP_ADJ_SCORE_LEVEL_2       (1740 + OM_BASIC_ADJ_IDS_SCORE_LEVEL_2)
#define SOUND_EFFECT_AP_ADJ_SCORE_LEVEL_3       (1740 + OM_BASIC_ADJ_IDS_SCORE_LEVEL_3)
#define SOUND_EFFECT_AP_SCORE_AWARDS            (1740 + OM_BASIC_ADJ_IDS_SCORE_AWARDS)
#define SOUND_EFFECT_AP_SCROLLING_SCORES        (1740 + OM_BASIC_ADJ_SCROLLING_SCORES)
#define SOUND_EFFECT_AP_ADJ_HISCR               (1740 + OM_BASIC_ADJ_IDS_HISCR)
#define SOUND_EFFECT_AP_ADJ_CREDITS             (1740 + OM_BASIC_ADJ_IDS_CREDITS)
#define SOUND_EFFECT_AP_ADJ_CPC_1               (1740 + OM_BASIC_ADJ_IDS_CPC_1)
#define SOUND_EFFECT_AP_ADJ_CPC_2               (1740 + OM_BASIC_ADJ_IDS_CPC_2)
#define SOUND_EFFECT_AP_ADJ_CPC_3               (1740 + OM_BASIC_ADJ_IDS_CPC_3)
#define SOUND_EFFECT_AP_MATCH_FEATURE           (1740 + OM_BASIC_ADJ_IDS_MATCH_FEATURE)

#define SOUND_EFFECT_OM_EASY_RULES_INSTRUCTIONS           1770
#define SOUND_EFFECT_OM_MEDIUM_RULES_INSTRUCTIONS         1771
#define SOUND_EFFECT_OM_HARD_RULES_INSTRUCTIONS           1772
#define SOUND_EFFECT_OM_PROGRESSIVE_RULES_INSTRUCTIONS    1773
#define SOUND_EFFECT_OM_CUSTOM_RULES_INSTRUCTIONS         1774

#define OM_GAME_ADJ_EASY_DIFFICULTY                 0
#define OM_GAME_ADJ_MEDIUM_DIFFICULTY               1
#define OM_GAME_ADJ_HARD_DIFFICULTY                 2
#define OM_GAME_ADJ_PROGRESSIVE_DIFFICULTY          3
#define OM_GAME_ADJ_CUSTOM_DIFFICULTY               4
#define SOUND_EFFECT_AP_DIFFICULTY                  (1790 + OM_GAME_ADJ_EASY_DIFFICULTY)

// CUSTOMIZATION TODO:
// Create defines for custom parameters and be sure to 
// create the associated callouts (starting at 1800)
#define OM_GAME_ADJ_TROUGH_EJECT_STRENGTH                   0
#define OM_GAME_ADJ_SAUCER_EJECT_STRENGTH                   1
#define OM_GAME_ADJ_APP_STARTS                              2
#define OM_GAME_ADJ_FINISHED                                3
#define SOUND_EFFECT_AP_GAME_ADJUSTMENT_CALLOUTS_START      (1800 + OM_GAME_ADJ_TROUGH_EJECT_STRENGTH)

unsigned long SoundSettingTimeout;
unsigned long SoundTestStart;
byte SoundTestSequence;
unsigned short RestoreBackgroundTrack = BACKGROUND_TRACK_NONE;
  
// The operator menu opens on its "return to game" page, which shows nothing: every display
// is blank and the game is stopped until a button is pressed. Left alone there it closes
// by itself, so a stray self-test signal can't leave the machine looking dead.
#define OPERATOR_MENU_IDLE_EXIT_MS    15000
unsigned long OperatorMenuLastActivity = 0;

void RunOperatorMenu() {
  boolean menuDone = !Menus.UpdateMenu(CurrentTime);

  if (!menuDone && Menus.GetTopLevel() == OPERATOR_MENU_RETURN_TO_GAME && OperatorMenuLastActivity != 0 &&
      (CurrentTime - OperatorMenuLastActivity) > OPERATOR_MENU_IDLE_EXIT_MS) {
    Menus.ExitOperatorMenu();
    menuDone = true;
  }

#ifdef RPU_OS_USE_PINNODE_LINK
  // From the phone, "exit diagnostics" closes the coin-door menu too; nothing else runs here
  PinNodeActionRequest menuRequest;
  if (!menuDone && PinNodeCtrl_TakeAction(&menuRequest)) {
    if (menuRequest.op == PINNODE_OP_START && menuRequest.action == ACT_EXIT_DIAGNOSTICS) {
      Menus.ExitOperatorMenu();
      menuDone = true;
      PinNodeCtrl_ActionResult(menuRequest.reqId, menuRequest.action, PINNODE_ACTION_DONE, 0, PINNODE_REASON_FINISHED);
    } else {
      PinNodeCtrl_ActionResult(menuRequest.reqId, menuRequest.action,
                               menuRequest.op == PINNODE_OP_STOP ? PINNODE_ACTION_DONE : PINNODE_ACTION_NOT_NOW, 0, menuRequest.reason);
    }
  }
#endif

  if (menuDone) {
    // Menu is done
    OperatorMenuLastActivity = 0;
#ifdef RPU_OS_USE_PINNODE_LINK
    // Tell the node what the operator changed (it diffs), and the audits (they may have been cleared)
    PinNodeCtrl_SettingsChanged();
    PinNodeCtrl_AuditsChanged();
#endif
    RPU_SetDisplayCredits(Credits, !FreePlayMode);
    Audio.StopAllAudio();
    RPU_TurnOffAllLamps();
    if (MachineState==MACHINE_STATE_ATTRACT) {
      RPU_SetDisplayBallInPlay(0, true);
    } else {
      RPU_SetDisplayBallInPlay(CurrentBallInPlay);
    }
    SoundSettingTimeout = 0;
    return;
  }

  // It's up to this function to eject balls if requested
  if (Menus.BallEjectInProgress()) {
    if (CountBallsInTrough()) {
      if (CurrentTime > (LastTimeBallServed+1500)) {
        LastTimeBallServed = CurrentTime;
        RPU_PushToSolenoidStack(SOL_SERVE_BALL, BallServeSolenoidStrength, true);
      }
    }
  } else {
    LastTimeBallServed = 0;
  }
  
  byte topLevel = Menus.GetTopLevel();
  byte subLevel = Menus.GetSubLevel();

  if (OperatorMenuLastActivity == 0) OperatorMenuLastActivity = CurrentTime;
  if (Menus.HasTopLevelChanged()) {
    OperatorMenuLastActivity = CurrentTime;
    // Play an audio prompt for the top level
    SoundTestStart = 0;
    if (Audio.GetBackgroundSong()!=BACKGROUND_TRACK_NONE) {
      RestoreBackgroundTrack = Audio.GetBackgroundSong();
    }
    Audio.StopAllAudio();
    Audio.PlaySound((unsigned short)topLevel + SOUND_EFFECT_AP_TOP_LEVEL_MENU_ENTRY, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
    if (Menus.GetTopLevel()==OPERATOR_MENU_GAME_RULES_LEVEL) Menus.SetNumSubLevels(4);
    if (Menus.GetTopLevel()==OPERATOR_MENU_BASIC_ADJ_MENU) {
      GetCPCSelection(0); // make sure CPC values have been read
      Menus.SetNumSubLevels(OM_BASIC_ADJ_FINISHED);
    }
    if (Menus.GetTopLevel()==OPERATOR_MENU_GAME_ADJ_MENU) Menus.SetNumSubLevels(OM_GAME_ADJ_FINISHED);
  }
  if (Menus.HasSubLevelChanged()) {
    OperatorMenuLastActivity = CurrentTime;
    SoundTestStart = 0;
    // Play an audio prompt for the sub level    
    Audio.StopAllAudio();
    if (topLevel==OPERATOR_MENU_SELF_TEST_MENU) {
      Audio.PlaySound((unsigned short)subLevel + SOUND_EFFECT_AP_TEST_LAMPS, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);

      if (subLevel==OPERATOR_MENU_TEST_SOUNDS) {
        SoundTestStart = CurrentTime + 1000;
        SoundTestSequence = 0;
      } else {
        SoundTestStart = 0;
      }
    } else if (topLevel==OPERATOR_MENU_AUDITS_MENU) {
      unsigned long *currentAdjustmentUL = NULL;
      byte currentAdjustmentStorageByte = 0;
      byte adjustmentType = OPERATOR_MENU_AUD_CLEARABLE;

      switch (subLevel) {
        case 0:
          Audio.PlaySound(SOUND_EFFECT_AP_AUDIT_TOTAL_PLAYS, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
          currentAdjustmentStorageByte = RPU_TOTAL_PLAYS_EEPROM_START_BYTE;
          break;
        case 1:
          Audio.PlaySound(SOUND_EFFECT_AP_AUDIT_CHUTE_1_COINS, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
          currentAdjustmentStorageByte = RPU_CHUTE_1_COINS_START_BYTE;
          break;
        case 2:
          Audio.PlaySound(SOUND_EFFECT_AP_AUDIT_CHUTE_2_COINS, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
          currentAdjustmentStorageByte = RPU_CHUTE_2_COINS_START_BYTE;
          break;
        case 3:
          Audio.PlaySound(SOUND_EFFECT_AP_AUDIT_CHUTE_3_COINS, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
          currentAdjustmentStorageByte = RPU_CHUTE_3_COINS_START_BYTE;
          break;
        case 4:
          Audio.PlaySound(SOUND_EFFECT_AP_AUDIT_TOTAL_REPLAYS, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
          currentAdjustmentStorageByte = RPU_TOTAL_REPLAYS_EEPROM_START_BYTE;
          break;
        case 5:
          Audio.PlaySound(SOUND_EFFECT_AP_AUDIT_HISCR_BEAT, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
          currentAdjustmentStorageByte = RPU_TOTAL_HISCORE_BEATEN_START_BYTE;
          break;
      }

      Menus.SetAuditControls(currentAdjustmentUL, currentAdjustmentStorageByte, adjustmentType);

    } else if (topLevel==OPERATOR_MENU_BASIC_ADJ_MENU) {
      Audio.PlaySound((unsigned short)subLevel + SOUND_EFFECT_AP_FREEPLAY, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);

      byte *currentAdjustmentByte = NULL;
      byte currentAdjustmentStorageByte = 0;
      byte adjustmentValues[8] = {0};
      byte numAdjustmentValues = 2;
      byte adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
      short parameterCallout = 0;
      unsigned long *currentAdjustmentUL = NULL;
      
      adjustmentValues[1] = 1;

      switch(subLevel) {
        case OM_BASIC_ADJ_IDS_FREEPLAY:
          currentAdjustmentByte = (byte *)&FreePlayMode;
          currentAdjustmentStorageByte = EEPROM_FREE_PLAY_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_BALL_SAVE:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_LIST;
          numAdjustmentValues = 5;
          adjustmentValues[1] = 5;
          adjustmentValues[2] = 10;
          adjustmentValues[3] = 15;
          adjustmentValues[4] = 20;
          currentAdjustmentByte = &BallSaveNumSeconds;
          currentAdjustmentStorageByte = EEPROM_BALL_SAVE_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_TILT_WARNINGS:
          adjustmentValues[1] = 2;
          currentAdjustmentByte = &MaxTiltWarnings;
          currentAdjustmentStorageByte = EEPROM_TILT_WARNING_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_MUSIC_VOLUME:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = 10;
          currentAdjustmentByte = &MusicVolume;
          currentAdjustmentStorageByte = EEPROM_MUSIC_VOLUME_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_SOUNDFX_VOLUME:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = 10;
          currentAdjustmentByte = &SoundEffectsVolume;
          currentAdjustmentStorageByte = EEPROM_SFX_VOLUME_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_CALLOUTS_VOLUME:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = 10;
          currentAdjustmentByte = &CalloutsVolume;
          currentAdjustmentStorageByte = EEPROM_CALLOUTS_VOLUME_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_BALLS_PER_GAME:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
          numAdjustmentValues = 8;
          adjustmentValues[0] = 3;
          adjustmentValues[1] = 10;
          currentAdjustmentByte = &BallsPerGame;
          currentAdjustmentStorageByte = EEPROM_BALLS_OVERRIDE_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_TOURNAMENT_MODE:
          currentAdjustmentByte = (byte *)&TournamentScoring;
          currentAdjustmentStorageByte = EEPROM_TOURNAMENT_SCORING_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_EXTRA_BALL_VALUE:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_SCORE_WITH_DEFAULT;
          currentAdjustmentUL = &ExtraBallValue;
          currentAdjustmentStorageByte = EEPROM_EXTRA_BALL_SCORE_UL;
          break;
        case OM_BASIC_ADJ_IDS_SPECIAL_VALUE:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_SCORE_WITH_DEFAULT;
          currentAdjustmentUL = &SpecialValue;
          currentAdjustmentStorageByte = EEPROM_SPECIAL_SCORE_UL;
          break;
        case OM_BASIC_ADJ_IDS_RESET_DURING_GAME:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_LIST;
          numAdjustmentValues = 5;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = 1;
          adjustmentValues[2] = 2;
          adjustmentValues[3] = 3;
          adjustmentValues[4] = 99;
          currentAdjustmentByte = &TimeRequiredToResetGame;
          currentAdjustmentStorageByte = EEPROM_CRB_HOLD_TIME;
          parameterCallout = SOUND_EFFECT_OM_CRB_VALUES;
          break;
        case OM_BASIC_ADJ_IDS_SCORE_LEVEL_1:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_SCORE_WITH_DEFAULT;
          currentAdjustmentUL = &AwardScores[0];
          currentAdjustmentStorageByte = RPU_AWARD_SCORE_1_EEPROM_START_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_SCORE_LEVEL_2:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_SCORE_WITH_DEFAULT;
          currentAdjustmentUL = &AwardScores[1];
          currentAdjustmentStorageByte = RPU_AWARD_SCORE_2_EEPROM_START_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_SCORE_LEVEL_3:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_SCORE_WITH_DEFAULT;
          currentAdjustmentUL = &AwardScores[2];
          currentAdjustmentStorageByte = RPU_AWARD_SCORE_3_EEPROM_START_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_SCORE_AWARDS:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX_DEFAULT;
          adjustmentValues[1] = 7;
          currentAdjustmentByte = &ScoreAwardReplay;
          currentAdjustmentStorageByte = EEPROM_AWARD_OVERRIDE_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_SCROLLING_SCORES:
          currentAdjustmentByte = (byte *)&ScrollingScores;
          currentAdjustmentStorageByte = EEPROM_SCROLLING_SCORES_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_HISCR:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_SCORE_WITH_DEFAULT;
          currentAdjustmentUL = &HighScore;
          currentAdjustmentStorageByte = RPU_HIGHSCORE_EEPROM_START_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_CREDITS:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = 40;
          currentAdjustmentByte = &Credits;
          currentAdjustmentStorageByte = RPU_CREDITS_EEPROM_BYTE;
          break;
        case OM_BASIC_ADJ_IDS_CPC_1:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_CPC;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = (NUM_CPC_PAIRS-1);
          currentAdjustmentByte = &(CPCSelection[0]);
          currentAdjustmentStorageByte = RPU_CPC_CHUTE_1_SELECTION_BYTE;
          parameterCallout = SOUND_EFFECT_OM_CPC_VALUES;
          break;
        case OM_BASIC_ADJ_IDS_CPC_2:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_CPC;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = (NUM_CPC_PAIRS-1);
          currentAdjustmentByte = &(CPCSelection[1]);
          currentAdjustmentStorageByte = RPU_CPC_CHUTE_2_SELECTION_BYTE;
          parameterCallout = SOUND_EFFECT_OM_CPC_VALUES;
          break;
        case OM_BASIC_ADJ_IDS_CPC_3:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_CPC;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = (NUM_CPC_PAIRS-1);
          currentAdjustmentByte = &(CPCSelection[2]);
          currentAdjustmentStorageByte = RPU_CPC_CHUTE_3_SELECTION_BYTE;
          parameterCallout = SOUND_EFFECT_OM_CPC_VALUES;
          break;
        case OM_BASIC_ADJ_IDS_MATCH_FEATURE:
          currentAdjustmentByte = (byte *)&MatchFeature;
          currentAdjustmentStorageByte = EEPROM_MATCH_FEATURE_BYTE;
          break;
      }

      Menus.SetParameterControls(   adjustmentType, numAdjustmentValues, adjustmentValues, parameterCallout,
                                    currentAdjustmentStorageByte, currentAdjustmentByte, currentAdjustmentUL );
#ifdef RPU_OS_USE_PINNODE_LINK
      GuardAdjustmentIfLocked(currentAdjustmentStorageByte, currentAdjustmentByte, currentAdjustmentUL);
#endif
    } else if (topLevel==OPERATOR_MENU_GAME_RULES_LEVEL) {
      Audio.PlaySound((unsigned short)subLevel + SOUND_EFFECT_AP_DIFFICULTY, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
      byte *currentAdjustmentByte = &GameRulesSelection;
      byte adjustmentValues[8] = {0};
      adjustmentValues[0] = 0;
      // if one of the below parameters is installed, the "HasParameterChanged" 
      // check below will install Easy / Medium / Hard rules

      switch (subLevel) {
        case 0:
          adjustmentValues[1] = 1;
          break;
        case 1:
          adjustmentValues[1] = 2;
          break;
        case 2:
          adjustmentValues[1] = 3;
          break;
        case 3:
          adjustmentValues[1] = 4;
          break;
      }

      Menus.SetParameterControls(   OPERATOR_MENU_ADJ_TYPE_LIST, 2, adjustmentValues, (short)SOUND_EFFECT_OM_EASY_RULES_INSTRUCTIONS-1,
                                    EEPROM_GAME_RULES_SELECTION, currentAdjustmentByte, NULL );
#ifdef RPU_OS_USE_PINNODE_LINK
      GuardAdjustmentIfLocked(EEPROM_GAME_RULES_SELECTION, currentAdjustmentByte, NULL);
#endif
                  
    } else if (topLevel==OPERATOR_MENU_GAME_ADJ_MENU) {
      Audio.PlaySound((unsigned short)subLevel + SOUND_EFFECT_AP_GAME_ADJUSTMENT_CALLOUTS_START, AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);

      byte *currentAdjustmentByte = NULL;
      byte currentAdjustmentStorageByte = 0;
      byte adjustmentValues[8] = {0};
      byte numAdjustmentValues = 2;
      byte adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
      short parameterCallout = 0;
      unsigned long *currentAdjustmentUL = NULL;
      
      adjustmentValues[1] = 1;

      // CUSTOMIZATION TODO:
      // Add adjustment values here for game rules

      switch (subLevel) {
        case OM_GAME_ADJ_TROUGH_EJECT_STRENGTH:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
          adjustmentValues[0] = 3;
          adjustmentValues[1] = 10;
          currentAdjustmentByte = &BallServeSolenoidStrength;
          currentAdjustmentStorageByte = EEPROM_TROUGH_EJECT_STRENGTH;
          break;
        case OM_GAME_ADJ_SAUCER_EJECT_STRENGTH:
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
          adjustmentValues[0] = 5;
          adjustmentValues[1] = 15;
          currentAdjustmentByte = &SaucerSolenoidStrength;
          currentAdjustmentStorageByte = EEPROM_SAUCER_EJECT_STRENGTH;
          break;
        case OM_GAME_ADJ_APP_STARTS:
          // 0: the machine takes no starts from the PinNode app; 1: it takes them like the start button
          adjustmentType = OPERATOR_MENU_ADJ_TYPE_MIN_MAX;
          adjustmentValues[0] = 0;
          adjustmentValues[1] = 1;
          currentAdjustmentByte = (byte *)&AppStartsEnabled;
          currentAdjustmentStorageByte = EEPROM_APP_STARTS_BYTE;
          break;
      }
      
      Menus.SetParameterControls(   adjustmentType, numAdjustmentValues, adjustmentValues, parameterCallout,
                                    currentAdjustmentStorageByte, currentAdjustmentByte, currentAdjustmentUL );
#ifdef RPU_OS_USE_PINNODE_LINK
      GuardAdjustmentIfLocked(currentAdjustmentStorageByte, currentAdjustmentByte, currentAdjustmentUL);
#endif
    }    
  }

  boolean parameterChanged = Menus.HasParameterChanged();
  if (parameterChanged) OperatorMenuLastActivity = CurrentTime;
#ifdef RPU_OS_USE_PINNODE_LINK
  // a difficulty setting under a tournament lock goes back to what it was
  if (parameterChanged && RevertGuardedAdjustment()) parameterChanged = false;
#endif
  if (parameterChanged) {
    short parameterCallout = Menus.GetParameterCallout();
    if (parameterCallout) {
      Audio.StopAllAudio();
      Audio.PlaySound((unsigned short)parameterCallout + Menus.GetParameterID(), AUDIO_PLAY_TYPE_WAV_TRIGGER, 10);
    }
    if (Menus.GetTopLevel()==OPERATOR_MENU_GAME_RULES_LEVEL) {
      // Install the new rules level
      if (LoadRuleDefaults(GameRulesSelection)) {
        WriteParameters();
      }
    } else if (Menus.GetTopLevel()==OPERATOR_MENU_BASIC_ADJ_MENU) {
      if (Menus.GetSubLevel()==OM_BASIC_ADJ_IDS_MUSIC_VOLUME) {
        if (SoundSettingTimeout) Audio.StopAllAudio();
        Audio.PlaySound(SOUND_EFFECT_BACKGROUND_1, AUDIO_PLAY_TYPE_WAV_TRIGGER, MusicVolume);
        Audio.SetMusicVolume(MusicVolume);
        SoundSettingTimeout = CurrentTime + 5000;
      } else if (Menus.GetSubLevel()==OM_BASIC_ADJ_IDS_SOUNDFX_VOLUME) {
        if (SoundSettingTimeout) Audio.StopAllAudio();
        Audio.PlaySound(SOUND_EFFECT_PHASER_1, AUDIO_PLAY_TYPE_WAV_TRIGGER, SoundEffectsVolume);
        Audio.SetSoundFXVolume(SoundEffectsVolume);
        SoundSettingTimeout = CurrentTime + 5000;
      } else if (Menus.GetSubLevel()==OM_BASIC_ADJ_IDS_CALLOUTS_VOLUME) {
        if (SoundSettingTimeout) Audio.StopAllAudio();
        Audio.PlaySound(SOUND_EFFECT_PLAYER_1_UP, AUDIO_PLAY_TYPE_WAV_TRIGGER, CalloutsVolume);
        Audio.SetNotificationsVolume(CalloutsVolume);
        SoundSettingTimeout = CurrentTime + 3000;        
      }
    } else if (Menus.GetTopLevel()==OPERATOR_MENU_GAME_ADJ_MENU) {
    }
  }

  if (SoundSettingTimeout && CurrentTime>SoundSettingTimeout) {
    SoundSettingTimeout = 0;
    Audio.StopAllAudio();
  }

  if (SoundTestStart && CurrentTime>SoundTestStart) {
    if (SoundTestSequence==0) {
      PlayBackgroundSong(SOUND_EFFECT_BACKGROUND_1);
      SoundTestSequence = 1;
    } else if (SoundTestSequence==1 && CurrentTime>(SoundTestStart+5000)) {
      PlaySoundEffect(SOUND_EFFECT_PHASER_1);
      SoundTestSequence = 2;
    } else if (SoundTestSequence==2 && CurrentTime>(SoundTestStart+10000)) {
      Audio.QueuePrioritizedNotification(SOUND_EFFECT_PLAYER_1_UP, 3000, 10, CurrentTime);
      SoundTestSequence = 3;
    }
  }
  
}



////////////////////////////////////////////////////////////////////////////
//
//  Audio Output functions
//
////////////////////////////////////////////////////////////////////////////
void PlayBackgroundSong(unsigned int songNum) {

  if (MusicVolume == 0) return;

  Audio.PlayBackgroundSong(songNum);

}


unsigned long NextSoundEffectTime = 0;

void PlaySoundEffect(unsigned int soundEffectNum) {

  if (MachineState == MACHINE_STATE_INIT_GAMEPLAY) return;

  // Play digital samples on the WAV trigger (numbered same
  // as SOUND_EFFECT_ defines)
  Audio.PlaySound(soundEffectNum, AUDIO_PLAY_TYPE_WAV_TRIGGER);

  // SOUND_EFFECT_ defines can also be translated into
  // commands for the sound card
  switch (soundEffectNum) {
      /*
          case SOUND_EFFECT_LEFT_SHOOTER_LANE:
            Audio.PlaySoundCardWhenPossible(12, CurrentTime, 0, 500, 7);
            break;
          case SOUND_EFFECT_RETURN_TO_SHOOTER_LANE:
            Audio.PlaySoundCardWhenPossible(22, CurrentTime, 0, 500, 8);
            break;
          case SOUND_EFFECT_SAUCER:
            Audio.PlaySoundCardWhenPossible(14, CurrentTime, 0, 500, 7);
            break;
          case SOUND_EFFECT_DROP_TARGET_HURRY:
            Audio.PlaySoundCardWhenPossible(2, CurrentTime, 0, 45, 3);
            break;
          case SOUND_EFFECT_DROP_TARGET_COMPLETE:
            Audio.PlaySoundCardWhenPossible(9, CurrentTime, 0, 1400, 4);
            Audio.PlaySoundCardWhenPossible(19, CurrentTime, 1500, 10, 4);
            break;
          case SOUND_EFFECT_HOOFBEATS:
            Audio.PlaySoundCardWhenPossible(12, CurrentTime, 0, 100, 10);
            break;
          case SOUND_EFFECT_STOP_BACKGROUND:
            Audio.PlaySoundCardWhenPossible(19, CurrentTime, 0, 10, 10);
            break;
          case SOUND_EFFECT_DROP_TARGET_HIT:
            Audio.PlaySoundCardWhenPossible(7, CurrentTime, 0, 150, 5);
            break;
          case SOUND_EFFECT_SPINNER:
            Audio.PlaySoundCardWhenPossible(6, CurrentTime, 0, 25, 2);
            break;
      */
  }
}


// How long each voice callout runs, in seconds (tracks 100-110), so the queue and the
// music ducking don't depend on the WAV Trigger reporting back what is playing.
byte VoiceNotificationDurations[NUM_VOICE_NOTIFICATIONS] = {
  2, 2, 2, 2, 3, 3, 3, 3, 4, 3, 4
};

void QueueNotification(unsigned int soundEffectNum, byte priority) {
  if (CalloutsVolume == 0) return;

  unsigned short duration = 0;
  if (soundEffectNum >= SOUND_EFFECT_VP_VOICE_NOTIFICATIONS_START && soundEffectNum < (SOUND_EFFECT_VP_VOICE_NOTIFICATIONS_START + NUM_VOICE_NOTIFICATIONS)) {
    duration = 1000 * (unsigned short)VoiceNotificationDurations[soundEffectNum - SOUND_EFFECT_VP_VOICE_NOTIFICATIONS_START];
  }
  Audio.QueuePrioritizedNotification(soundEffectNum, duration, priority, CurrentTime);
}


void AlertPlayerUp() {
  QueueNotification(SOUND_EFFECT_PLAYER_1_UP + CurrentPlayer, 1);
}




////////////////////////////////////////////////////////////////////////////
//
//  Diagnostics Mode
//
////////////////////////////////////////////////////////////////////////////

// The diagnostics state: where the tests asked for from the phone run. The app enters and
// leaves it with the two diagnostics actions; the self-test or credit button at the
// machine leaves it too. After five minutes with no test and no request, the PinNode
// library asks for the exit action itself (when the phone was what entered diagnostics).
int RunDiagnosticsMode(int curState, boolean curStateChanged) {

  int returnState = curState;

#ifdef RPU_OS_USE_PINNODE_LINK
  if (curStateChanged) {
    RPU_DisableSolenoidStack();
    RPU_SetDisableFlippers(true);
    RPU_TurnOffAllLamps();
    Audio.StopAllAudio();
    Display_ClearOverride(0xFF);
    for (byte count = 0; count < RPU_NUMBER_OF_PLAYER_DISPLAYS; count++) RPU_SetDisplayBlank(count, 0x00);
    RPU_SetDisplayCredits(0, false);
    RPU_SetDisplayBallInPlay(0, false);
    RemoteTest = REMOTE_TEST_NONE;
  }

  returnState = HandleOperatorAction(curState);
  if (returnState != curState) return returnState;

  UpdateRemoteTest();

  byte switchHit;
  while ( (switchHit = RPU_PullFirstFromSwitchStack()) != SWITCH_STACK_EMPTY ) {
    if (switchHit == SW_SELF_TEST_SWITCH || (switchHit == SW_CREDIT_RESET && RemoteTest != ACT_SWITCH_TEST)) {
      returnState = MACHINE_STATE_ATTRACT;
    }
  }

  if (returnState != curState) EndDiagnostics();
#else
  (void)curStateChanged;
  returnState = MACHINE_STATE_ATTRACT;
#endif

  return returnState;
}




////////////////////////////////////////////////////////////////////////////
//
//  Attract Mode
//
////////////////////////////////////////////////////////////////////////////
byte AttractLastHeadMode = 255;
byte AttractLastPlayfieldMode = 255;
boolean AttractCheckedForTrappedBall;
unsigned long AttractModeStartTime;

int RunAttractMode(int curState, boolean curStateChanged) {

  int returnState = curState;

  if (curStateChanged) {
    // Some sound cards have a special index
    // for a "sound" that will turn off
    // the current background drone or currently
    // playing sound
    RPU_DisableSolenoidStack();
    RPU_TurnOffAllLamps();
    RPU_SetDisableFlippers(true);
    if (DEBUG_MESSAGES) {
      Serial.write("Entering Attract Mode\n\r");
    }
    AttractLastHeadMode = 0;
    RPU_SetDisplayCredits(Credits, !FreePlayMode);
    Display_ClearOverride(0xFF);
    Display_UpdateDisplays(0xFF);
    AttractCheckedForTrappedBall = false;
    AttractModeStartTime = CurrentTime;

#ifdef RPU_OS_USE_ACCESSORY_LAMP_BOARD
    TopperALB.StopAnimation();
    TopperALB.PlayAnimation(GI_0_SET_COLOR, 250, 0, 0);
    TopperALB.PlayAnimation(GI_1_SET_COLOR, 150, 27, 0);
#endif
  }

  if (CurrentTime > (AttractModeStartTime + 5000) && !AttractCheckedForTrappedBall) {
    AttractCheckedForTrappedBall = true;
    if (DEBUG_MESSAGES) {
      Serial.write("In Attract for 10 seconds - make sure there are no balls trapped\n");
    }

    if (RPU_ReadSingleSwitchState(SW_SAUCER)) {
      RPU_PushToSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, true);
    }
  }

  //RPU_SetLampState(LAMP_APRON_CREDITS, (FreePlayMode || Credits) ? true : false, 0, 200);

  // Claim codes: after a game the node sends a 6-digit code for each player nobody checked
  // in as. The PinNode library shows it in that player's display (the code blinking for
  // 5 seconds, the score for 2, the player-up lamp flashing with the code) and says which
  // displays it has: nothing else draws those on this pass. (It lights its lamp again on
  // every pass, so the attract show turning lamps off doesn't stop the flashing.)
  byte claimDisplays = 0;
#ifdef RPU_OS_USE_PINNODE_LINK
  claimDisplays = PinNode_ShowClaimCodes(CurrentTime, CurrentScores, PlayerUpLamps);
#endif

  // Alternate displays between high score and blank
  if (CurrentTime < 16000) {
    if (AttractLastHeadMode != 1) {
      RPU_SetDisplayCredits(Credits, !FreePlayMode);
      RPU_SetDisplayBallInPlay(0, true);
      Display_ClearOverride(0xFF);
    }
    AttractLastHeadMode = 1;
    for (byte count = 0; count < RPU_NUMBER_OF_PLAYER_DISPLAYS; count++) {
      if (!(claimDisplays & (1 << count))) Display_UpdateDisplays(count);
    }
    RPU_SetLampState(LAMP_HEAD_HIGH_SCORE, 0);
#ifdef RPU_OS_USE_PINNODE_LINK
  // (the high score takes all four displays, so it waits while a claim code is up)
  } else if ((CurrentTime / 8000) % 2 == 0 && !PinNode_ClaimCodesShowing()) {
#else
  } else if ((CurrentTime / 8000) % 2 == 0) {
#endif

    if (AttractLastHeadMode != 2) {
      Display_SetLastTimeScoreChanged(CurrentTime);
    }
    AttractLastHeadMode = 2;
    for (byte count = 0; count < RPU_NUMBER_OF_PLAYER_DISPLAYS; count++) {
      if (!(claimDisplays & (1 << count))) Display_UpdateDisplays(count, false, false, false, HighScore);
    }
    RPU_SetLampState(LAMP_HEAD_HIGH_SCORE, 1, 0, 250);
    for (byte count=0; count<4; count++) {
      // (not a lamp the claim display has: this runs after it in the pass, and would win)
      if (!(claimDisplays & (1 << count))) RPU_SetLampState(PlayerUpLamps[count], 0);
    }
  } else {
    if (AttractLastHeadMode != 3) {
      if (CurrentTime < 32000) {
        for (int count = 0; count < RPU_NUMBER_OF_PLAYERS_ALLOWED; count++) {
          CurrentScores[count] = 0;
        }
        CurrentNumPlayers = 0;
      }
      Display_SetLastTimeScoreChanged(CurrentTime);
    }

    RPU_SetLampState(LAMP_HEAD_HIGH_SCORE, 0);
    for (byte count=0; count<4; count++) {
      if (claimDisplays & (1 << count)) continue;
      if (count<CurrentNumPlayers) Display_UpdateDisplays(count, false, false, false, CurrentScores[count]);
      else if (CurrentNumPlayers==0) Display_UpdateDisplays(count, false, false, false, 0);
      else RPU_SetDisplayBlank(count, 0x00);   // not in the last game: don't leave the high score showing there
    }

    AttractLastHeadMode = 3;
  }

  RPU_SetLampState(LAMP_HEAD_GAME_OVER, AttractLastHeadMode != 2);

  // The 2022 attract show: five animations, five seconds each
  byte attractPlayfieldPhase = ((CurrentTime / 5000) % 5);
  if (attractPlayfieldPhase != AttractLastPlayfieldMode) {
    RPU_TurnOffAllLamps();
    AttractLastPlayfieldMode = attractPlayfieldPhase;
  }

  if (attractPlayfieldPhase == 1) {
    ShowLampAnimation(1, 40, CurrentTime, 14, false, false);
  } else if (attractPlayfieldPhase == 3) {
    ShowLampAnimation(0, 40, CurrentTime, 11, false, false);
  } else if (attractPlayfieldPhase == 2) {
    ShowLampAnimation(3, 40, CurrentTime, 14, false, false);
  } else if (attractPlayfieldPhase == 0) {
    ShowLampAnimation(0, 40, CurrentTime, 14, false, true);
  } else {
    ShowLampAnimation(2, 40, CurrentTime, 14, false, false);
  }

#ifdef RPU_OS_USE_PINNODE_LINK
  // Operator actions from the phone: one-shots, and entering diagnostics
  returnState = HandleOperatorAction(curState);

  // Start pressed in the app (after a check-in): a new game, as with the credit button.
  // It spends a credit unless free play is on. With "starts from the app" off, the machine
  // takes none (the library answers for it).
  uint16_t startRequest;
  if (PinNodeCtrl_TakeAppStart(&startRequest, AppStartsEnabled)) {
    if (AddPlayer(true)) {
      returnState = MACHINE_STATE_INIT_GAMEPLAY;
      PinNodeCtrl_StartResult(startRequest, PINNODE_START_OK, 1);
    } else {
      PinNodeCtrl_StartResult(startRequest, PINNODE_START_NO_CREDITS, 0);
    }
  }
#endif

  byte switchHit;
  while ( (switchHit = RPU_PullFirstFromSwitchStack()) != SWITCH_STACK_EMPTY ) {
    if (switchHit == SW_CREDIT_RESET) {
      if (AddPlayer(true)) returnState = MACHINE_STATE_INIT_GAMEPLAY;
    }
    if (switchHit == SW_COIN_1 || switchHit == SW_COIN_2 || switchHit == SW_COIN_3 ) {
      AddCoinToAudit(SwitchToChuteNum(switchHit));
      AddCoin(SwitchToChuteNum(switchHit));
#ifdef RPU_OS_USE_PINNODE_LINK
      PinNodeCtrl_Coin(SwitchToChuteNum(switchHit) + 1, Credits);   // credits after the coin
#endif
    }
    if (switchHit == SW_SELF_TEST_SWITCH) {
      Menus.EnterOperatorMenu();
    }
  }

  // If the user was holding the menu button when the game started
  // then kick the balls
  if (CurrentTime < 4000) {
    if (RPU_ReadSingleSwitchState(SW_SELF_TEST_SWITCH)) {
      if (OperatorSwitchPressStarted==0) {
        OperatorSwitchPressStarted = CurrentTime;
      } else if (CurrentTime > (OperatorSwitchPressStarted+500)) {
        Menus.EnterOperatorMenu();
        Menus.BallEjectInProgress(true);
      }
    } else {
      OperatorSwitchPressStarted = 0;
    }
  }

  return returnState;
}





////////////////////////////////////////////////////////////////////////////
//
//  Game Play functions
//
////////////////////////////////////////////////////////////////////////////
byte CountBits(unsigned short intToBeCounted) {
  byte numBits = 0;

  for (byte count = 0; count < 16; count++) {
    numBits += (intToBeCounted & 0x01);
    intToBeCounted = intToBeCounted >> 1;
  }

  return numBits;
}


void SetGameMode(byte newGameMode) {
  LastGameMode = GameMode;
  GameMode = newGameMode;
  GameModeStartTime = 0;
  GameModeEndTime = 0;

  if (DEBUG_MESSAGES) {
    char buf[128];
    sprintf(buf, "Game Mode = %d\n", newGameMode);
    Serial.write(buf);
  }
}

byte CountBallsInTrough() {

  byte numBalls = RPU_ReadSingleSwitchState(SW_OUTHOLE);

  return numBalls;
}



void AddToBonus(byte bonus) {
  Bonus[CurrentPlayer] += bonus;
  if (Bonus[CurrentPlayer] > MAX_DISPLAY_BONUS) {
    Bonus[CurrentPlayer] = MAX_DISPLAY_BONUS;
/*    
    // Some games award a special when
    // bonus is maxed out
    if (!SpecialCollected) {
      if (SpecialAvailable) {
        AwardSpecial();  
        SpecialAvailable = false;
      } else {
        SpecialAvailable = true;
      }
    }
*/    
  } else {
    BonusChanged = CurrentTime;
  }
}


void IncreaseBonusX() {
  if (BonusX[CurrentPlayer] < 3) {
    BonusX[CurrentPlayer] += 1;
    BonusXAnimationStart = CurrentTime;
    if (BonusX[CurrentPlayer] == 3) AwardAchievement(ACH_BONUS_3X);
  }
}



unsigned long GameStartNotificationTime = 0;
boolean WaitForBallToReachOuthole = false;
unsigned long UpperBallEjectTime = 0;

int InitGamePlay(boolean curStateChanged) {

  if (curStateChanged) {
    RPU_TurnOffAllLamps();
    SetGeneralIlluminationOn(true);
    GameStartNotificationTime = CurrentTime;
    Audio.StopAllAudio();
    QueueNotification(SOUND_EFFECT_ADD_PLAYER_1, 1);
    for (byte count = 0; count < RPU_NUMBER_OF_PLAYERS_ALLOWED; count++) RPU_SetDisplayBlank(count, 0x00);
    RPU_SetDisplayCredits(0, false);
    RPU_SetDisplayBallInPlay(1, true);
    NumberOfBallsLocked = 0;
#ifdef RPU_OS_USE_PINNODE_LINK
    PinNodeCtrl_GameStart(Credits, FreePlayMode);   // credits left after the start
    PinNode_ClaimCodesStop();   // the last game's codes are gone: no flashing lamp carries into play
#endif

    SaucerLetterIDX = 0;
    LaneLightPhase = false;
  }

  boolean showBIP = (CurrentTime / 100) % 2;
  RPU_SetDisplayBallInPlay(1, showBIP ? true : false);

  if (RPU_ReadSingleSwitchState(SW_SAUCER)) {
    RPU_PushToSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, true);
  }

  // The start button has been hit only once to get
  // us into this mode, so we assume a 1-player game
  // at the moment
  RPU_SetCoinLockout((Credits >= MaximumCredits) ? true : false);

  // Reset displays & game state variables
  for (int count = 0; count < RPU_NUMBER_OF_PLAYERS_ALLOWED; count++) {
    // Initialize game-specific variables
    Bonus[count] = 0;
    BonusX[count] = 1;
    CurrentAchievements[count] = 0;
    CurrentScores[count] = 0;
    BallyStatus[count] = 0;
    HSBonusIDX[count] = 0;
    MissionInProgress[count] = MISSION_NONE;
    MissionQualifiers[count] = 0;
    MissionsCompleted[count] = 0;
    AchievementsAwarded[count] = 0;
  }

  SamePlayerShootsAgain = false;
  CurrentBallInPlay = 1;
  CurrentNumPlayers = 1;
  CurrentPlayer = 0;
  NumberOfBallsInPlay = 0;
  LastTiltWarningTime = 0;
  Display_ClearOverride(0xFF);
  Display_UpdateDisplays(0xFF);
  RPU_EnableSolenoidStack();

  return MACHINE_STATE_INIT_NEW_BALL;
}



int InitNewBall(bool curStateChanged) {

  // If we're coming into this mode for the first time
  // then we have to do everything to set up the new ball
  if (curStateChanged) {

    if (GameRulesSelection==GAME_RULES_PROGRESSIVE) {
      // if the operator has installed "progressive" rules, 
      // then we change the rules based on player number.
      // If we're 1 player, it's EASY
      // First player is EASY, last player is HARD,
      // and in-between players are MEDIUM
      byte rulesLevel = GAME_RULES_EASY;
      if (CurrentPlayer==0) rulesLevel = GAME_RULES_EASY;
      else if (CurrentPlayer==(CurrentNumPlayers-1)) rulesLevel = GAME_RULES_HARD;
      else rulesLevel = GAME_RULES_MEDIUM;
      LoadRuleDefaults(rulesLevel);
      if (CurrentBallInPlay==1) QueueNotification(SOUND_EFFECT_VP_GAME_RULES_EASY + (rulesLevel-GAME_RULES_EASY), 10);
    }

    RPU_TurnOffAllLamps();
    BallFirstSwitchHitTime = 0;

    // The bonus countdown turns the solenoid stack off and a tilt turns the flippers off,
    // so every ball turns them back on (as RPU games do; ExampleMachine left this out).
    RPU_SetDisableFlippers(false);
    RPU_EnableSolenoidStack();
#ifdef RPU_OS_USE_PINNODE_LINK
    PinNodeCtrl_BallStart(CurrentPlayer + 1, CurrentBallInPlay);
#endif

    RPU_SetDisplayCredits(Credits, !FreePlayMode);
    if (CurrentNumPlayers > 1 && (CurrentBallInPlay != 1 || CurrentPlayer != 0) && !SamePlayerShootsAgain) AlertPlayerUp();
    SamePlayerShootsAgain = false;

    RPU_SetDisplayBallInPlay(CurrentBallInPlay);
    RPU_SetLampState(LAMP_HEAD_TILT, 0);
    for (byte count = 0; count < 4; count++) {
      if (count==CurrentPlayer) RPU_SetLampState(PlayerUpLamps[count], 1, 0, 250);
      else if (count<CurrentNumPlayers) RPU_SetLampState(PlayerUpLamps[count], 1);
      else RPU_SetLampState(PlayerUpLamps[count], 0);
      RPU_SetDisplayBlank(count, 0);
    }

    if (BallSaveNumSeconds > 0) {
      RPU_SetLampState(LAMP_SHOOT_AGAIN, 1, 0, 500);
    }

    BallSaveUsed = false;
    BallTimeInTrough = 0;
    NumTiltWarnings = 0;

    // Initialize game-specific start-of-ball lights & variables
    GameModeStartTime = 0;
    GameModeEndTime = 0;
    GameMode = GAME_MODE_SKILL_SHOT;

    SpecialCollected = false;
    SpecialAvailable = false;

    PlayfieldMultiplier = 1;
    PlayfieldMultiplierTimeLeft = 0;
    BonusXAnimationStart = 0;
    Bonus[CurrentPlayer] = 0;
    BonusX[CurrentPlayer] = 1;
    Display_ResetDisplayTrackingVariables();

    SetBallSave(0);

    // Star Trek: what starts over with each ball
    DTBonusIDX = 1;
    DTBonusMultiplier = 1;
    ToplaneProgress = 0;
    SkillShotLane = (CurrentTime / 7) % 2;
    ExtraBallCollected = false;
    HSEBQualified = false;
    BonusCollectedAtGate = false;
    BallSaveStarted = false;
    HyperSpaceExtraBonus = 0;
    HSrollovertimer = 0;
    BallyAnimationBegin = 0;
    LastSaucerHitTime = 0;
    SaucerKickTime = 0;
    LastTimeSaucerSeen = 0;
    HSLampLit = 0;
    HSLampAnimationDelay = 0;

    // Raise the drop targets and empty the saucer
    DropTargets.ResetDropTargets(CurrentTime + 100, true, true);
    if (RPU_ReadSingleSwitchState(SW_SAUCER)) {
      RPU_PushToTimedSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, CurrentTime + 300, true);
    }

    // Serve the ball. The first ball of a game waits for the new game sound.
    unsigned long serveDelay = 1000;
    IntroMusicEndTime = 0;
    if (CurrentBallInPlay == 1 && CurrentPlayer == 0 && CurrentScores[0] == 0) {
      serveDelay = 2500;
      // The new game theme is music: it plays at the music volume, and the background
      // loop waits until it has finished.
      if (MusicVolume) {
        Audio.PlaySound(SOUND_EFFECT_NEW_GAME, AUDIO_PLAY_TYPE_WAV_TRIGGER, MusicVolume);
        IntroMusicEndTime = CurrentTime + NEW_GAME_INTRO_MS;
      }
    }
    RPU_PushToTimedSolenoidStack(SOL_SERVE_BALL, BallServeSolenoidStrength, CurrentTime + serveDelay, true);
    LastTimeBallServed = CurrentTime + serveDelay;

    NumberOfBallsInPlay = 1;

    // A mission in progress carries over: its first phase starts again with this ball
    if (MissionInProgress[CurrentPlayer] == MISSION_DOOMSDAY) {
      PlayBackgroundSong(SOUND_DOOMSDAY_LEVEL_1);
      QueueNotification(SOUND_EFFECT_DOOMSDAY_PHASE1, 5);
      if (IntroMusicEndTime) Audio.StopSound(SOUND_EFFECT_NEW_GAME);
      IntroMusicEndTime = 0;
    } else if (IntroMusicEndTime == 0) {
      PlayBackgroundSong(SOUND_EFFECT_BACKGROUND_1);
    }
  }

  return MACHINE_STATE_NORMAL_GAMEPLAY;
  
  LastTimeThroughLoop = CurrentTime;
}




unsigned long SaucerClosedStart = 0;

// The saucer holds the ball until SaucerKickTime (set by the switch handler). If a ball
// sits there with no kick pending (it landed during a tilt, or the kick was weak), it's
// kicked again, once a second.
void ManageSaucer() {
  if (SaucerKickTime && CurrentTime >= SaucerKickTime) {
    SaucerKickTime = 0;
    RPU_PushToSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, true);
    SaucerClosedStart = CurrentTime;
  }

  if (RPU_ReadSingleSwitchState(SW_SAUCER)) {
    LastTimeSaucerSeen = CurrentTime;
    if (SaucerClosedStart == 0) {
      SaucerClosedStart = CurrentTime;
    } else if (SaucerKickTime == 0 && CurrentTime > (SaucerClosedStart + 3000)) {
      RPU_PushToSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, true);
      SaucerClosedStart = CurrentTime - 2000;
    }
  } else {
    SaucerClosedStart = 0;
  }
}

void UpdateDropTargets() {
  DropTargets.Update(CurrentTime);
}



unsigned long LastTimePromptPlayed = 0;
unsigned long LastTimeAwardsChecked = 0;
unsigned long LastTimeJackpotAdjusted = 0;
unsigned long LastLoopTick = 0;



// CUSTOMIZATION TODO:
// Put your game logic here.
// This function manages all timers, flags, and lights
// Normal play's background loop. It starts when the new game theme has finished.
void ManageBackgroundLoop() {
  if (IntroMusicEndTime != 0) {
    if (CurrentTime < IntroMusicEndTime) return;
    IntroMusicEndTime = 0;
  }
  if (Audio.GetBackgroundSong() != SOUND_EFFECT_BACKGROUND_1) PlayBackgroundSong(SOUND_EFFECT_BACKGROUND_1);
}


int ManageGameMode() {
  int returnState = MACHINE_STATE_NORMAL_GAMEPLAY;

  boolean specialAnimationRunning = false;

  UpdateDropTargets();

  if ((CurrentTime - LastSwitchHitTime) > 3000) TimersPaused = true;
  else TimersPaused = false;

  ManageSaucer();

  switch ( GameMode ) {
    case GAME_MODE_SKILL_SHOT:
      if (GameModeStartTime == 0) {
        GameModeStartTime = CurrentTime;
        GameModeEndTime = 0;
        LastTimePromptPlayed = CurrentTime;
      }

      // The switch handler awards the skill shot and leaves this mode
      // (LeaveSkillShot) on the first playfield switch.
      if (CurrentTime > (LastTimePromptPlayed + 20000)) {
        AlertPlayerUp();
        LastTimePromptPlayed = CurrentTime;
      }
      if (MissionInProgress[CurrentPlayer] == MISSION_NONE) ManageBackgroundLoop();

      if (BallFirstSwitchHitTime != 0) LeaveSkillShot();
      break;

    case GAME_MODE_UNSTRUCTURED_PLAY:
      if (GameModeStartTime == 0) {
        GameModeStartTime = CurrentTime;
      }
      ManageBackgroundLoop();

      // Mission start: this is where a qualified mission begins. Doomsday is the only one
      // built; the others will be chosen here (next uncompleted mission whose qualifiers are met).
      if (MissionInProgress[CurrentPlayer] == MISSION_NONE &&
          (MissionQualifiers[CurrentPlayer] & MISSION_QUALIFIERS_FOR_DOOMSDAY) == MISSION_QUALIFIERS_FOR_DOOMSDAY) {
        SetGameMode(GAME_MODE_DOOMSDAY_START);
      }
      break;

    case GAME_MODE_DOOMSDAY_START:
      if (GameModeStartTime == 0) {
        GameModeStartTime = CurrentTime;
        GameModeEndTime = CurrentTime + DOOMSDAY_START_DURATION;
        MissionInProgress[CurrentPlayer] = MISSION_DOOMSDAY;
        MissionQualifiers[CurrentPlayer] = 0;
        if (IntroMusicEndTime) Audio.StopSound(SOUND_EFFECT_NEW_GAME);   // the mission's music takes over
        IntroMusicEndTime = 0;
        PlayBackgroundSong(SOUND_DOOMSDAY_LEVEL_1);
        AwardAchievement(ACH_DOOMSDAY_STARTED);
      }
      if (CurrentTime >= GameModeEndTime) SetGameMode(GAME_MODE_DOOMSDAY_SHIELDS);
      break;

    case GAME_MODE_DOOMSDAY_SHIELDS:
      // Phase 1: the drop targets are locked (they reset when hit) until B-A-L-L-Y is
      // spelled. HandleBallyHit moves on to the attack.
      if (GameModeStartTime == 0) {
        GameModeStartTime = CurrentTime;
        GameModeEndTime = 0;
        RPU_SetDisplayCredits(Credits, !FreePlayMode);
        if (Audio.GetBackgroundSong() != SOUND_DOOMSDAY_LEVEL_1) PlayBackgroundSong(SOUND_DOOMSDAY_LEVEL_1);
        // a new ball has already announced phase 1 (InitNewBall)
        if (LastGameMode != GAME_MODE_SKILL_SHOT) QueueNotification(SOUND_EFFECT_DOOMSDAY_PHASE1, 5);
      }
      break;

    case GAME_MODE_DOOMSDAY_ATTACK:
      // Phase 2: drop all four targets before the timer runs out. HandleDropTargetHit
      // moves on to the finish; a timeout puts the shields back up.
      if (GameModeStartTime == 0) {
        GameModeStartTime = CurrentTime;
        GameModeEndTime = CurrentTime + DOOMSDAY_ATTACK_DURATION;
        PlayBackgroundSong(SOUND_DOOMSDAY_LEVEL_2);
        QueueNotification(SOUND_EFFECT_DOOMSDAY_PHASE2, 5);
      }

      if (CurrentTime >= GameModeEndTime) {
        RPU_SetDisplayCredits(Credits, !FreePlayMode);
        SetGameMode(GAME_MODE_DOOMSDAY_SHIELDS);
      } else {
        // the seconds left, in the credit display
        RPU_SetDisplayCredits((GameModeEndTime - CurrentTime) / 1000, true);
      }
      break;

    case GAME_MODE_DOOMSDAY_FINISH:
      if (GameModeStartTime == 0) {
        GameModeStartTime = CurrentTime;
        GameModeEndTime = CurrentTime + DOOMSDAY_FINISH_DURATION;
        MissionInProgress[CurrentPlayer] = MISSION_NONE;
        MissionsCompleted[CurrentPlayer] |= (1 << (MISSION_DOOMSDAY - 1));
        RPU_SetDisplayCredits(Credits, !FreePlayMode);
        AwardExtraBall();
        Display_StartScoreAnimation(DOOMSDAY_COMPLETE_AWARD, true);
        PlayBackgroundSong(SOUND_EFFECT_BACKGROUND_1);
        QueueNotification(SOUND_EFFECT_DOOMSDAY_COMPLETE, 5);
        AwardAchievement(ACH_DOOMSDAY_COMPLETE);
        AddToStat(STAT_MISSIONS_COMPLETED);
      }
      if (CurrentTime >= GameModeEndTime) SetGameMode(GAME_MODE_UNSTRUCTURED_PLAY);
      break;

    default:
      // A mode nobody handles would leave the game stuck: go back to normal play
      SetGameMode(GAME_MODE_UNSTRUCTURED_PLAY);
      break;
  }

  if ( !specialAnimationRunning && NumTiltWarnings <= MaxTiltWarnings ) {
    ShowBonusLamps();
    ShowBonusXLamps();
    ShowShootAgainLamp();
    ShowBallyLamps();
    ShowLaneLamps();
    ShowHyperSpaceLamps();
    ShowDTBonusLamps();
    ShowPlayerLamps();
  }

  if (Display_UpdateDisplays(0xFF, false, (BallFirstSwitchHitTime == 0) ? true : false, (BallFirstSwitchHitTime > 0 && ((CurrentTime - Display_GetLastTimeScoreChanged()) > 2000)) ? true : false)) {
    Audio.StopSound(SOUND_EFFECT_ANIMATION_TICK);
    PlaySoundEffect(SOUND_EFFECT_ANIMATION_TICK);
  }

  // Check to see if ball is in the outhole
  // (not until the serve has had its chance: the ball waits in the outhole to be served)
  if (CountBallsInTrough() > (TotalBallsLoaded - (NumberOfBallsInPlay + NumberOfBallsLocked)) && CurrentTime > (LastTimeBallServed + 500)) {

    if (BallTimeInTrough == 0) {
      // If this is the first time we're seeing too many balls in the trough, we'll wait to make sure
      // everything is settled
      BallTimeInTrough = CurrentTime;
    } else {

      // Make sure the ball stays on the sensor for at least
      // 0.5 seconds to be sure that it's not bouncing or passing through
      if ((CurrentTime - BallTimeInTrough) > 750) {

        if ((BallFirstSwitchHitTime == 0 && NumTiltWarnings <= MaxTiltWarnings)) {
          // Nothing hit yet, so return the ball to the player
          RPU_PushToTimedSolenoidStack(SOL_SERVE_BALL, BallServeSolenoidStrength, CurrentTime);
          BallTimeInTrough = 0;
          returnState = MACHINE_STATE_NORMAL_GAMEPLAY;
        } else {
          // if we haven't used the ball save, and we're under the time limit, then save the ball
          if (BallSaveEndTime && CurrentTime < (BallSaveEndTime + BALL_SAVE_GRACE_PERIOD)) {
            RPU_PushToTimedSolenoidStack(SOL_SERVE_BALL, BallServeSolenoidStrength, CurrentTime + 100);

            RPU_SetLampState(LAMP_SHOOT_AGAIN, 0);
            BallTimeInTrough = CurrentTime;
            returnState = MACHINE_STATE_NORMAL_GAMEPLAY;

            if (NumberOfBallSavesRemaining && NumberOfBallSavesRemaining != 0xFF) {
              NumberOfBallSavesRemaining -= 1;
              if (NumberOfBallSavesRemaining == 0) {
                BallSaveEndTime = 0;
                if (DEBUG_MESSAGES) {
                  Serial.write("Last ball save\n");
                }
                PlaySoundEffect(SOUND_EFFECT_SHOOT_AGAIN);
              } else {
                if (DEBUG_MESSAGES) {
                  Serial.write("Not last ball save\n");
                }                
              }
            }

          } else {

            NumberOfBallsInPlay -= 1;
            if (NumberOfBallsInPlay == 0) {
              Display_ClearOverride(0xFF);
              Audio.StopAllAudio();
              //PlaySoundEffect(SOUND_EFFECT_BALL_OVER);
              returnState = MACHINE_STATE_COUNTDOWN_BONUS;
            }
          }
        }
      }
    }
  } else {
    BallTimeInTrough = 0;
  }

  LastLoopTick = CurrentTime;
  LastTimeThroughLoop = CurrentTime;
  return returnState;
}



unsigned long CountdownStartTime = 0;
unsigned long LastCountdownReportTime = 0;
unsigned long BonusCountDownEndTime = 0;
unsigned long BonusCountdownDelay = 250;

// 1,000 x the bonus multiplier for each bonus unit, at a steady pace set by the size of
// the bonus when the countdown starts (a bigger bonus counts faster)
int CountdownBonus(boolean curStateChanged) {

  // If this is the first time through the countdown loop
  if (curStateChanged) {

    // Turn off solenoids
    RPU_DisableSolenoidStack();

    if (RPU_ReadSingleSwitchState(SW_SAUCER)) {
      RPU_PushToSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, true);
    }

    CountdownStartTime = CurrentTime;
    LastCountdownReportTime = CurrentTime;
    ShowBonusLamps();
    BonusCountDownEndTime = 0xFFFFFFFF;

    long countdownDelayTime = 250 - (((long)Bonus[CurrentPlayer]) * 2 * ((long)BonusX[CurrentPlayer]));
    if (countdownDelayTime < 20) countdownDelayTime = 20;
    BonusCountdownDelay = (unsigned long)countdownDelayTime;

    if (NumTiltWarnings <= MaxTiltWarnings && Bonus[CurrentPlayer]) {
      RecordStatMax(STAT_BEST_BONUS, ((long)Bonus[CurrentPlayer]) * 1000L * ((long)BonusX[CurrentPlayer]));
    }
  }

  if ((CurrentTime - LastCountdownReportTime) > BonusCountdownDelay) {

    if (Bonus[CurrentPlayer]) {

      // Only give sound & score if this isn't a tilt
      if (NumTiltWarnings <= MaxTiltWarnings) {
        PlaySoundEffect(SOUND_EFFECT_ANIMATION_TICK);
        CurrentScores[CurrentPlayer] += 1000 * ((unsigned long)BonusX[CurrentPlayer]);
      }

      Bonus[CurrentPlayer] -= 1;
      ShowBonusLamps();

    } else if (BonusCountDownEndTime == 0xFFFFFFFF) {
      BonusCountDownEndTime = CurrentTime + 1000;
    }
    LastCountdownReportTime = CurrentTime;
  }

  if (CurrentTime > BonusCountDownEndTime) {
    BonusCountDownEndTime = 0xFFFFFFFF;
    return MACHINE_STATE_BALL_OVER;
  }

  return MACHINE_STATE_COUNTDOWN_BONUS;
}



void CheckHighScores() {
  unsigned long highestScore = 0;
  int highScorePlayerNum = 0;
  for (int count = 0; count < CurrentNumPlayers; count++) {
    if (CurrentScores[count] > highestScore) {
      highestScore = CurrentScores[count];
      highScorePlayerNum = count;
    }
  }

  if (highestScore > HighScore) {
    HighScore = highestScore;
    if (HighScoreReplay) {
      AddCredit(false, 3);
      RPU_WriteULToEEProm(RPU_TOTAL_REPLAYS_EEPROM_START_BYTE, RPU_ReadULFromEEProm(RPU_TOTAL_REPLAYS_EEPROM_START_BYTE) + 3);
    }
    RPU_WriteULToEEProm(RPU_HIGHSCORE_EEPROM_START_BYTE, highestScore);
    RPU_WriteULToEEProm(RPU_TOTAL_HISCORE_BEATEN_START_BYTE, RPU_ReadULFromEEProm(RPU_TOTAL_HISCORE_BEATEN_START_BYTE) + 1);

    for (int count = 0; count < RPU_NUMBER_OF_PLAYERS_ALLOWED; count++) {
      if (count == highScorePlayerNum) {
        RPU_SetDisplay(count, CurrentScores[count], true, 2);
      } else {
        RPU_SetDisplayBlank(count, 0x00);
      }
    }

    RPU_PushToTimedSolenoidStack(SOL_KNOCKER, KNOCKER_SOLENOID_STRENGTH, CurrentTime, true);
    RPU_PushToTimedSolenoidStack(SOL_KNOCKER, KNOCKER_SOLENOID_STRENGTH, CurrentTime + 300, true);
    RPU_PushToTimedSolenoidStack(SOL_KNOCKER, KNOCKER_SOLENOID_STRENGTH, CurrentTime + 600, true);
  }
}


unsigned long MatchSequenceStartTime = 0;
unsigned long MatchDelay = 150;
byte MatchDigit = 0;
byte NumMatchSpins = 0;
byte ScoreMatches = 0;

int ShowMatchSequence(boolean curStateChanged) {
  if (!MatchFeature) return MACHINE_STATE_ATTRACT;

  if (curStateChanged) {
    MatchSequenceStartTime = CurrentTime;
    MatchDelay = 1500;
    MatchDigit = CurrentTime % 10;
    NumMatchSpins = 0;
    RPU_SetLampState(LAMP_HEAD_MATCH, 1, 0);
    RPU_SetDisableFlippers();
    ScoreMatches = 0;
  }

  if (NumMatchSpins < 40) {

    if (NumMatchSpins<10) {
      byte blankMask = (0x3F << ((NumMatchSpins)/2)) & 0x3F;
      RPU_SetDisplayBlank(0, RPU_GetDisplayBlank(0) & blankMask);
      RPU_SetDisplayBlank(1, RPU_GetDisplayBlank(1) & blankMask);      
    } else if (NumMatchSpins>16 && NumMatchSpins<40) {
      if (CurrentNumPlayers>2) {
        unsigned long scoreMatch1 = CurrentScores[0] % 100;
        unsigned long scoreMatch2 = CurrentScores[1] % 100;
        unsigned long scoreMatch3 = CurrentScores[2] % 100;
        unsigned long scoreMatch4 = CurrentScores[3] % 100;
        unsigned long display1, display2;
        byte display1Mask, display2Mask;
        byte shiftPhase = (NumMatchSpins - 16)/3;
        switch (shiftPhase) {
          case 0:
            display1 = 10*scoreMatch1;
            display1Mask = 0x18;
            display2 = 10*scoreMatch2;
            display2Mask = 0x18;
            break;
          case 1:
            display1 = 100*scoreMatch1;
            display1Mask = 0x0C;
            display2 = 100*scoreMatch2;
            display2Mask = 0x0C;
            break;
          case 2:
            display1 = 1000*scoreMatch1;
            display1Mask = 0x06;
            display2 = 1000*scoreMatch2 + scoreMatch3/10;
            display2Mask = 0x26;
            break;
          case 3:
            display1 = 10000*scoreMatch1;
            display1Mask = 0x03;
            display2 = 10000*scoreMatch2 + scoreMatch3;
            display2Mask = 0x33;
            break;
          case 4:
            display1 = 10000*scoreMatch1;
            display1Mask = 0x03;
            display2 = scoreMatch3 * 10;
            display2Mask = 0x18;
            break;
          case 5:
            display1 = 10000*scoreMatch1;
            display1Mask = 0x03;
            display2 = 100*scoreMatch3;
            display2Mask = 0x0C;
            break;
          case 6:
            display1 = 10000*scoreMatch1 + scoreMatch2/10;
            display1Mask = 0x23;
            display2 = 1000*scoreMatch3;
            display2Mask = 0x06;
            if (CurrentNumPlayers==4) {
              display2 += scoreMatch4 / 10;
              display2Mask = 0x26;
            }
            break;
          case 7:
            display1 = 10000*scoreMatch1 + scoreMatch2;
            display1Mask = 0x33;
            display2 = 10000*scoreMatch3;
            display2Mask = 0x03;
            if (CurrentNumPlayers==4) {
              display2 += scoreMatch4;
              display2Mask = 0x33;
            }            
            break;
        }
        RPU_SetDisplayBlank(0, display1Mask);
        RPU_SetDisplayBlank(1, display2Mask);
        RPU_SetDisplay(0, display1);
        RPU_SetDisplay(1, display2);        
      }
    }
    
    if (CurrentTime > (MatchSequenceStartTime + MatchDelay)) {
      MatchDigit += 1;
      if (MatchDigit > 9) MatchDigit = 0;
      if (NumMatchSpins == 0) PlaySoundEffect(SOUND_EFFECT_MATCH_SPIN);
      RPU_SetDisplayBallInPlay((int)MatchDigit * 10);
      MatchDelay += 50 + 4 * NumMatchSpins;
      NumMatchSpins += 1;
      RPU_SetLampState(LAMP_HEAD_MATCH, NumMatchSpins % 2, 0);

      if (NumMatchSpins == 40) {
        Audio.StopSound(SOUND_EFFECT_MATCH_SPIN);
        RPU_SetLampState(LAMP_HEAD_MATCH, 0);
        MatchDelay = CurrentTime - MatchSequenceStartTime;
      }
    }
  }

  if (NumMatchSpins >= 40 && NumMatchSpins <= 43) {
    if (CurrentTime > (MatchSequenceStartTime + MatchDelay)) {
      if ( (CurrentNumPlayers > (NumMatchSpins - 40)) && ((CurrentScores[NumMatchSpins - 40] / 10) % 10) == MatchDigit) {
        ScoreMatches |= (1 << (NumMatchSpins - 40));
        AddSpecialCredit();
        MatchDelay += 1000;
        NumMatchSpins += 1;
        RPU_SetLampState(LAMP_HEAD_MATCH, 1);
      } else {
        NumMatchSpins += 1;
      }
      if (NumMatchSpins == 44) {
        MatchDelay += 5000;
      }
    }
  }

  if (NumMatchSpins > 43) {
    if (CurrentTime > (MatchSequenceStartTime + MatchDelay)) {
      return MACHINE_STATE_ATTRACT;
    }
  }

  for (int count = 0; count < RPU_NUMBER_OF_PLAYERS_ALLOWED; count++) {
    if (CurrentNumPlayers<3) {
      if ((ScoreMatches >> count) & 0x01) {
        // If this score matches, we're going to flash the last two digits
        byte upperMask = 0x0F;
        byte lowerMask = 0x30;
        if (RPU_OS_NUM_DIGITS == 7) {
          upperMask = 0x1F;
          lowerMask = 0x60;
        }
        if ( (CurrentTime / 200) % 2 ) {
          RPU_SetDisplayBlank(count, RPU_GetDisplayBlank(count) & upperMask);
        } else {
          RPU_SetDisplayBlank(count, RPU_GetDisplayBlank(count) | lowerMask);
        }
      }
    } else {
      // With more than 2 players, the scores will be compressed
      if ((ScoreMatches >> count) & 0x01) {
        byte displayMask0 = 0, displayMask1 = 0;
        if (RPU_OS_NUM_DIGITS == 7) {
          switch (count) {
            case 0: displayMask0 = 0x03; break;
            case 1: displayMask0 = 0x60; break;
            case 2: displayMask1 = 0x03; break;
            case 3: displayMask1 = 0x60; break;
          }
        } else {
          switch (count) {
            case 0: displayMask0 = 0x03; break;
            case 1: displayMask0 = 0x30; break;
            case 2: displayMask1 = 0x03; break;
            case 3: displayMask1 = 0x30; break;
          }
        }
        
        if ( (CurrentTime / 200) % 2 ) {
          RPU_SetDisplayBlank(0, RPU_GetDisplayBlank(0) & ~displayMask0);
          RPU_SetDisplayBlank(1, RPU_GetDisplayBlank(1) & ~displayMask1);
        } else {
          RPU_SetDisplayBlank(0, RPU_GetDisplayBlank(0) | displayMask0);
          RPU_SetDisplayBlank(1, RPU_GetDisplayBlank(1) | displayMask1);
        }
      }
    }
  }

  return MACHINE_STATE_MATCH_MODE;
}




////////////////////////////////////////////////////////////////////////////
//
//  Switch Handling functions
//
////////////////////////////////////////////////////////////////////////////
/*
  // Example lock function

  void HandleLockSwitch(byte lockIndex) {

  if (GameMode==GAME_MODE_UNSTRUCTURED_PLAY) {
    // If this player has a lock available
    if (PlayerLockStatus[CurrentPlayer] & (LOCK_1_AVAILABLE<<lockIndex)) {
      // Lock the ball
      LockBall(lockIndex);
      SetGameMode(GAME_MODE_OFFER_LOCK);
    } else {
      if ((MachineLocks & (LOCK_1_ENGAGED<<lockIndex))==0) {
        // Kick unlocked ball
        RPU_PushToSolenoidStack(SOL_UPPER_BALL_EJECT, 12, true);
      }
    }
  }
  }

*/

unsigned long LastSelfTestSwitchTime = 0;

int HandleSystemSwitches(int curState, byte switchHit) {
  int returnState = curState;
  switch (switchHit) {
    case SW_SELF_TEST_SWITCH:
      // During a game the menu needs two presses, between 0.3 and 3 seconds apart. The
      // self-test switch is read from an interrupt line, not the switch matrix, and one
      // stray pulse on it used to stop the game and blank the displays.
      if (LastSelfTestSwitchTime != 0 && (CurrentTime - LastSelfTestSwitchTime) > 300 && (CurrentTime - LastSelfTestSwitchTime) < 3000) {
        LastSelfTestSwitchTime = 0;
        Menus.EnterOperatorMenu();
      } else {
        LastSelfTestSwitchTime = CurrentTime;
      }
      break;
    case SW_COIN_1:
    case SW_COIN_2:
    case SW_COIN_3:
      AddCoinToAudit(SwitchToChuteNum(switchHit));
      AddCoin(SwitchToChuteNum(switchHit));
#ifdef RPU_OS_USE_PINNODE_LINK
      PinNodeCtrl_Coin(SwitchToChuteNum(switchHit) + 1, Credits);   // credits after the coin
#endif
      break;
    case SW_CREDIT_RESET:
      if (MachineState == MACHINE_STATE_MATCH_MODE) {
        // If the first ball is over, pressing start again resets the game
        if (Credits >= 1 || FreePlayMode) {
          if (!FreePlayMode) {
            Credits -= 1;
            RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
            RPU_SetDisplayCredits(Credits, !FreePlayMode);
          }
          returnState = MACHINE_STATE_INIT_GAMEPLAY;
        }
      } else {
        CreditResetPressStarted = CurrentTime;
      }
      break;
    case SW_OUTHOLE:
      // Some machines have a kicker to move the ball
      // from the outhole to the re-shooter ramp
      break;
//  case SW_TILT:      
//  case SW_ROLL_TILT:      
    case SW_PLUMB_TILT:
      if (BallFirstSwitchHitTime) {
        if ( CurrentTime > (LastTiltWarningTime + TILT_WARNING_DEBOUNCE_TIME) ) {
          LastTiltWarningTime = CurrentTime;
          NumTiltWarnings += 1;
#ifdef RPU_OS_USE_PINNODE_LINK
          PinNodeCtrl_Tilt(CurrentPlayer + 1, NumTiltWarnings > MaxTiltWarnings ? PINNODE_TILT_TILT : PINNODE_TILT_WARNING);
#endif
          if (NumTiltWarnings > MaxTiltWarnings) {
            SetGeneralIlluminationOn(false);
            RPU_DisableSolenoidStack();
            RPU_SetDisableFlippers(true);
            RPU_TurnOffAllLamps();
            RPU_SetLampState(LAMP_HEAD_TILT, 1);
            SaucerKickTime = 0;
            if (RPU_ReadSingleSwitchState(SW_SAUCER)) RPU_PushToSolenoidStack(SOL_SAUCER, SaucerSolenoidStrength, true);
            Audio.StopAllAudio();
            if (BallSaveEndTime) {
              BallSaveEndTime = 0;
              NumberOfBallSavesRemaining = 0;
            }
            PlaySoundEffect(SOUND_EFFECT_TILT);
          } else {
            PlaySoundEffect(SOUND_EFFECT_TILT_WARNING);
          }
        }
      } else {
        // Tilt before ball is plunged -- show a timer in ManageGameMode if desired
        if ( CurrentTime > (LastTiltWarningTime + TILT_WARNING_DEBOUNCE_TIME) ) {
          PlaySoundEffect(SOUND_EFFECT_TILT_WARNING);
        }
        LastTiltWarningTime = CurrentTime;
      }
      break;
  }

  return returnState;
}







// The first playfield switch of a ball ends the skill shot: on to the mission in
// progress (its first phase), or to normal play.
void LeaveSkillShot() {
  if (GameMode != GAME_MODE_SKILL_SHOT) return;
  Display_ClearOverride(0xFF);
  if (MissionInProgress[CurrentPlayer] == MISSION_DOOMSDAY) SetGameMode(GAME_MODE_DOOMSDAY_SHIELDS);
  else SetGameMode(GAME_MODE_UNSTRUCTURED_PLAY);
}


void ValidateAndRegisterPlayfieldSwitch() {
  LastSwitchHitTime = CurrentTime;
  if (BallFirstSwitchHitTime == 0) BallFirstSwitchHitTime = CurrentTime;
  LeaveSkillShot();
}


// A B-A-L-L-Y letter: the two top rollovers, the three standup targets, or the saucer
// spotting the letter its lamp shows.
void HandleBallyHit(byte switchNum) {
  if (switchNum > SW_B_ROLLOVER || switchNum < SW_Y_TARGET) return;
  byte letterByte = LetterToByteConversion[SW_B_ROLLOVER - switchNum];

  if ((BallyStatus[CurrentPlayer] & letterByte) == 0) {
    BallyStatus[CurrentPlayer] |= letterByte;
    CurrentScores[CurrentPlayer] += 1000 * PlayfieldMultiplier;
    AddToBonus(1);
  }

  if (BallyStatus[CurrentPlayer] == 0x1F) {
    Display_StartScoreAnimation(30000 * PlayfieldMultiplier, true);
    BallyAnimationBegin = CurrentTime;
    PlaySoundEffect(SOUND_SPOCK_FASCINATING);
    BallyStatus[CurrentPlayer] = 0;
    AddToBonus(30);
    IncreaseBonusX();
    AwardAchievement(ACH_BALLY);
    AddToStat(STAT_BALLY_SPELLED);

    if (MissionInProgress[CurrentPlayer] == MISSION_NONE) {
      MissionQualifiers[CurrentPlayer] |= MISSION_QUALIFIER_BALLY;
    } else if (GameMode == GAME_MODE_DOOMSDAY_SHIELDS) {
      // Doomsday phase 1 done: the shields are down
      SetGameMode(GAME_MODE_DOOMSDAY_ATTACK);
    }
  }
}


void RotateBallyLetters() {
  BallyStatus[CurrentPlayer] = ((BallyStatus[CurrentPlayer] << 1) | (BallyStatus[CurrentPlayer] >> 4)) & 0x1F;
}


// The two top lanes, B and A
void HandleTopLane(byte switchHit, boolean skillShotActive) {
  byte lane = (switchHit == SW_B_ROLLOVER) ? 0 : 1;

  if (skillShotActive && lane == SkillShotLane) {
    PlaySoundEffect(SOUND_EFFECT_SKILL_SHOT);
    Display_StartScoreAnimation(10000 * PlayfieldMultiplier, true);
    AwardAchievement(ACH_SKILL_SHOT);
    AddToStat(STAT_SKILL_SHOTS);
  } else {
    PlaySoundEffect(SOUND_EFFECT_PHASER_1);
    CurrentScores[CurrentPlayer] += 100 * PlayfieldMultiplier;
    // a lane already made this ball is worth 5,000
    if (ToplaneProgress & (1 << lane)) Display_StartScoreAnimation(5000 * PlayfieldMultiplier, true);
    else CurrentScores[CurrentPlayer] += 100 * PlayfieldMultiplier;
  }
  ToplaneProgress |= (1 << lane);

  HandleBallyHit(switchHit);
}


// Hyperspace: the lower rollover and then the upper one within a second
void HandleHSRolloverHit(byte switchNum) {
  if (switchNum == SW_LOWER_HS_ROLLOVER) {
    HSrollovertimer = CurrentTime;
  } else if (switchNum == SW_UPPER_HS_ROLLOVER && HSrollovertimer != 0 && (CurrentTime - HSrollovertimer) < 1000 && HSBonusIDX[CurrentPlayer] < 7) {
    HSrollovertimer = 0;
    HSBonusIDX[CurrentPlayer] += 1;
    PlaySoundEffect(SOUND_EFFECT_WARP);
    AwardAchievement(ACH_HYPERSPACE);
    AddToStat(STAT_HYPERSPACE_LOOPS);

    // with the hyperspace bonus window open, this lights the saucer for 15,000
    if (HyperSpaceExtraBonus != 0) HSEBQualified = true;

    if (HSBonusIDX[CurrentPlayer] == 6) {
      AwardExtraBall();
      AwardAchievement(ACH_HYPERSPACE_LADDER);
      HSBonusIDX[CurrentPlayer] = 0;    // the ladder starts over after the extra ball
    }
    AddToBonus(HSBonusIDX[CurrentPlayer]);
  }
}


boolean DropTargetsLocked() {
  // Doomsday: the targets can only be dropped during the attack
  return (MissionInProgress[CurrentPlayer] == MISSION_DOOMSDAY && GameMode != GAME_MODE_DOOMSDAY_ATTACK);
}


void HandleDropTargetHit(byte switchHit) {

  if (DropTargetsLocked()) {
    DropTargets.ResetDropTargets(CurrentTime + 200, true);
    return;
  }

  byte newTargets = DropTargets.HandleDropTargetHit(switchHit);
  if (newTargets) {
    CurrentScores[CurrentPlayer] += PlayfieldMultiplier * 500;
    PlaySoundEffect(SOUND_EFFECT_DROP_TARGET_1 + CurrentTime % 3);
    AddToBonus(1);
  }

  if (DropTargets.CheckIfBankCleared()) {
    PlaySoundEffect(SOUND_EFFECT_DROP_TARGET_COMPLETE);
    Display_StartScoreAnimation(((unsigned long)DTBonusValue[DTBonusIDX]) * 1000, true);
    AddToBonus(DTBonusValue[DTBonusIDX]);
    AwardAchievement(ACH_DROP_BANK);
    AddToStat(STAT_DROP_BANKS);
    if (DTBonusIDX == 4) {
      AwardExtraBall();
      AwardAchievement(ACH_DROP_LADDER);
    }
    if (DTBonusMultiplier < 3) DTBonusMultiplier += 1;
    if (DTBonusIDX < 4) DTBonusIDX += 1;
    DropTargets.ResetDropTargets(CurrentTime + 750, true);

    if (MissionInProgress[CurrentPlayer] == MISSION_NONE) {
      MissionQualifiers[CurrentPlayer] |= MISSION_QUALIFIER_DROP_BANK;
    } else if (GameMode == GAME_MODE_DOOMSDAY_ATTACK) {
      // Doomsday phase 2 done: the planet killer is destroyed
      SetGameMode(GAME_MODE_DOOMSDAY_FINISH);
    }
  }
}


void HandleSaucerHit() {
  // The switch chatters while the ball settles
  if (LastSaucerHitTime != 0 && (CurrentTime - LastSaucerHitTime) <= 900) return;
  LastSaucerHitTime = CurrentTime;
  LastTimeSaucerSeen = CurrentTime;

  if (HSEBQualified && HyperSpaceExtraBonus != 0 && (CurrentTime - HyperSpaceExtraBonus) < 30000) {
    Display_StartScoreAnimation(15000 * PlayfieldMultiplier, true);
    SaucerKickTime = CurrentTime + 2000;
    HSEBQualified = false;
    AwardAchievement(ACH_HYPERSPACE_SAUCER);
  } else {
    CurrentScores[CurrentPlayer] += 2000 * PlayfieldMultiplier;
    PlaySoundEffect(SOUND_EFFECT_RED_ALERT);
    SaucerKickTime = CurrentTime + 1000;
  }
  HyperSpaceExtraBonus = 0;

  // spot the letter the saucer lamp shows (B first, then A, L, L, Y as the bumpers rotate it)
  HandleBallyHit(SW_B_ROLLOVER - SaucerLetterIDX);
  AddToBonus(1);
}


// Where every playfield switch scores. The values, sounds and lamps are the 2022 code's.
void HandleGamePlaySwitches(byte switchHit) {

  boolean skillShotActive = (GameMode == GAME_MODE_SKILL_SHOT);

  switch (switchHit) {

    case SW_A_ROLLOVER:
    case SW_B_ROLLOVER:
      ValidateAndRegisterPlayfieldSwitch();
      HandleTopLane(switchHit, skillShotActive);
      break;

    case SW_LEFT_BUMPER:
    case SW_RIGHT_BUMPER:
    case SW_CENTER_BUMPER:
      ValidateAndRegisterPlayfieldSwitch();
      CurrentScores[CurrentPlayer] += 100 * PlayfieldMultiplier;
      PlaySoundEffect(SOUND_EFFECT_BUMPER_HIT_1 + CurrentTime % 3);
      break;

    case SW_L_TARGET:
    case SW_2L_TARGET:
    case SW_Y_TARGET:
      ValidateAndRegisterPlayfieldSwitch();
      CurrentScores[CurrentPlayer] += 300 * PlayfieldMultiplier;
      PlaySoundEffect(SOUND_EFFECT_PHOTON_TORPEDO_1 + CurrentTime % 3);
      HandleBallyHit(switchHit);
      break;

    case SW_LEFT_MIDDLE_LANE:
    case SW_RIGHT_MIDDLE_LANE:
      ValidateAndRegisterPlayfieldSwitch();
      // the lit lane (the bumpers' advance switch moves the light) opens the hyperspace bonus window
      if ((switchHit == SW_LEFT_MIDDLE_LANE) ? !LaneLightPhase : LaneLightPhase) {
        AddToBonus(5);
        CurrentScores[CurrentPlayer] += 5000 * PlayfieldMultiplier;
        HyperSpaceExtraBonus = CurrentTime;
      } else {
        CurrentScores[CurrentPlayer] += 500 * PlayfieldMultiplier;
      }
      break;

    case SW_LOWER_HS_ROLLOVER:
    case SW_UPPER_HS_ROLLOVER:
      ValidateAndRegisterPlayfieldSwitch();
      HandleHSRolloverHit(switchHit);
      break;

    case SW_UPPER_500_TARGET:
      ValidateAndRegisterPlayfieldSwitch();
      AddToBonus(1);
      CurrentScores[CurrentPlayer] += 500 * PlayfieldMultiplier;
      PlaySoundEffect(SOUND_EFFECT_TOP_TARGET);
      break;

    case SW_3K_TARGET:
      ValidateAndRegisterPlayfieldSwitch();
      AddToBonus(1);
      CurrentScores[CurrentPlayer] += 300 * PlayfieldMultiplier;
      PlaySoundEffect(SOUND_EFFECT_CENTER_TARGET);
      break;

    case SW_LEFT_INNNER_LANE:
    case SW_RIGHT_INNNER_LANE:
      ValidateAndRegisterPlayfieldSwitch();
      PlaySoundEffect(SOUND_EFFECT_INNER_LANE);
      if (LaneLightPhase) {
        AddToBonus(1);
        CurrentScores[CurrentPlayer] += 300 * PlayfieldMultiplier;
      }
      break;

    case SW_BALL_RETURN:
      // The ball is back in the shooter lane. The first time each ball, the bonus is
      // collected now (and kept for the end of the ball too). A drain from the shooter
      // lane with nothing hit serves the ball again; the ball save doesn't start over.
      if (!skillShotActive && BallFirstSwitchHitTime != 0) {
        CurrentScores[CurrentPlayer] += 1000 * PlayfieldMultiplier;
        if (!BonusCollectedAtGate) {
          BonusCollectedAtGate = true;
          Display_StartScoreAnimation(((unsigned long)Bonus[CurrentPlayer]) * 1000 * ((unsigned long)BonusX[CurrentPlayer]), true);
        }
        PlaySoundEffect(SOUND_EFFECT_TRANSPORTER);
        BallFirstSwitchHitTime = 0;
      }
      break;

    case SW_RIGHT_OUTLANE:
    case SW_LEFT_OUTLANE:
      ValidateAndRegisterPlayfieldSwitch();
      PlaySoundEffect(SOUND_EFFECT_OUTTERLANE);
      if (!LaneLightPhase) {
        CurrentScores[CurrentPlayer] += 1000 * PlayfieldMultiplier;
        AddToBonus(1);
      }
      SetBallSave(3000, 0, true);   // 3 more seconds of ball save, if it's still running
      break;

    case SW_ADV_BONUS_BUMPERS:
      ValidateAndRegisterPlayfieldSwitch();
      LaneLightPhase = !LaneLightPhase;
      RotateBallyLetters();
      SaucerLetterIDX = (SaucerLetterIDX + 1) % 5;
      PlaySoundEffect(SOUND_EFFECT_ROTATE_BALLY_1 + CurrentTime % 3);
      break;

    case SW_DROP_1:
    case SW_DROP_2:
    case SW_DROP_3:
    case SW_DROP_4:
      ValidateAndRegisterPlayfieldSwitch();
      HandleDropTargetHit(switchHit);
      break;

    case SW_LEFT_SLINGSHOT:
    case SW_RIGHT_SLINGSHOT:
      ValidateAndRegisterPlayfieldSwitch();
      CurrentScores[CurrentPlayer] += 10 * PlayfieldMultiplier;
      PlaySoundEffect(SOUND_EFFECT_SLING_SHOT);
      break;

    case SW_SAUCER:
      ValidateAndRegisterPlayfieldSwitch();
      HandleSaucerHit();
      break;
  }

}


int RunGamePlayMode(int curState, boolean curStateChanged) {
  int returnState = curState;
  unsigned long scoreAtTop = CurrentScores[CurrentPlayer];

  // Very first time into gameplay loop
  if (curState == MACHINE_STATE_INIT_GAMEPLAY) {
    returnState = InitGamePlay(curStateChanged);
  } else if (curState == MACHINE_STATE_INIT_NEW_BALL) {
    returnState = InitNewBall(curStateChanged);
  } else if (curState == MACHINE_STATE_NORMAL_GAMEPLAY) {
    returnState = ManageGameMode();
  } else if (curState == MACHINE_STATE_COUNTDOWN_BONUS) {
    Display_ClearOverride(0xFF);
    Display_UpdateDisplays(0xFF, true);
    returnState = CountdownBonus(curStateChanged);
  } else if (curState == MACHINE_STATE_BALL_OVER) {
    RPU_SetDisplayCredits(Credits, !FreePlayMode);
#ifdef RPU_OS_USE_PINNODE_LINK
    // A saved ball never gets here (ManageGameMode re-serves it), so this is the real end of
    // the ball, after the bonus count. Stats sent before BallEnd go out ahead of it.
    PinNodeCtrl_BallEnd(CurrentPlayer + 1, CurrentBallInPlay, CurrentScores[CurrentPlayer]);
#endif

    if (SamePlayerShootsAgain) {
      PlaySoundEffect(SOUND_EFFECT_SHOOT_AGAIN);
      returnState = MACHINE_STATE_INIT_NEW_BALL;
    } else {

      CurrentPlayer += 1;
      if (CurrentPlayer >= CurrentNumPlayers) {
        CurrentPlayer = 0;
        CurrentBallInPlay += 1;
      }

      scoreAtTop = CurrentScores[CurrentPlayer];

      if (CurrentBallInPlay > BallsPerGame) {
#ifdef RPU_OS_USE_PINNODE_LINK
        PinNodeCtrl_GameEnd(CurrentScores, CurrentNumPlayers, PINNODE_END_NORMAL);
#endif
        CheckHighScores();
        PlaySoundEffect(SOUND_EFFECT_GAME_OVER);
        for (int count = 0; count < CurrentNumPlayers; count++) {
          RPU_SetDisplay(count, CurrentScores[count], true, 2);
        }

        for (byte count = 0; count < 4; count++) {
          RPU_SetLampState(PlayerUpLamps[count], 0);
        }

        if (MatchEnabled) {
          returnState = MACHINE_STATE_MATCH_MODE;
        } else {
          returnState = MACHINE_STATE_ATTRACT;
        }
      }
      else returnState = MACHINE_STATE_INIT_NEW_BALL;
    }
  } else if (curState == MACHINE_STATE_MATCH_MODE) {
    returnState = ShowMatchSequence(curStateChanged);
  }

  byte switchHit;
  unsigned long lastBallFirstSwitchHitTime = BallFirstSwitchHitTime;

  while ( (switchHit = RPU_PullFirstFromSwitchStack()) != SWITCH_STACK_EMPTY ) {
    returnState = HandleSystemSwitches(curState, switchHit);
    if (NumTiltWarnings <= MaxTiltWarnings) HandleGamePlaySwitches(switchHit);
  }

#ifdef RPU_OS_USE_PINNODE_LINK
  // Start pressed in the app during a game: add a player, as with the credit button
  uint16_t startRequest;
  if (PinNodeCtrl_TakeAppStart(&startRequest, AppStartsEnabled)) {
    if (CurrentBallInPlay >= 2) PinNodeCtrl_StartResult(startRequest, PINNODE_START_NOT_NOW, 0);
    else if (CurrentNumPlayers >= RPU_NUMBER_OF_PLAYERS_ALLOWED) PinNodeCtrl_StartResult(startRequest, PINNODE_START_MAX_PLAYERS, 0);
    else if (AddPlayer()) PinNodeCtrl_StartResult(startRequest, PINNODE_START_OK, CurrentNumPlayers);
    else PinNodeCtrl_StartResult(startRequest, PINNODE_START_NO_CREDITS, 0);
  }
#endif

  if (CreditResetPressStarted) {
    if (CurrentBallInPlay < 2) {
      // If we haven't finished the first ball, we can add players
      AddPlayer();
      if (DEBUG_MESSAGES) {
        Serial.write("Start game button pressed\n\r");
      }
      CreditResetPressStarted = 0;
    } else {
      if (RPU_ReadSingleSwitchState(SW_CREDIT_RESET)) {
        if (TimeRequiredToResetGame != 99 && (CurrentTime - CreditResetPressStarted) >= ((unsigned long)TimeRequiredToResetGame * 1000)) {
          // If the first ball is over, pressing start again resets the game
          if (Credits >= 1 || FreePlayMode) {
            if (!FreePlayMode) {
              Credits -= 1;
              RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
              RPU_SetDisplayCredits(Credits, !FreePlayMode);
            }
#ifdef RPU_OS_USE_PINNODE_LINK
            // The game is abandoned for a new one: close it so the node doesn't wait for it
            PinNodeCtrl_GameEnd(CurrentScores, CurrentNumPlayers, PINNODE_END_RESET);
#endif
            returnState = MACHINE_STATE_INIT_GAMEPLAY;
            CreditResetPressStarted = 0;
          }
        }
      } else {
        CreditResetPressStarted = 0;
      }
    }

  }

  // (with ball save off there is no grace period either)
  // (and once per ball: the ball return gate doesn't start it again)
  if (lastBallFirstSwitchHitTime == 0 && BallFirstSwitchHitTime != 0 && BallSaveNumSeconds > 0 && !BallSaveStarted) {
    BallSaveStarted = true;
    BallSaveEndTime = BallFirstSwitchHitTime + ((unsigned long)BallSaveNumSeconds) * 1000;
    NumberOfBallSavesRemaining = 1;
  }
  if (CurrentTime > (BallSaveEndTime + BALL_SAVE_GRACE_PERIOD)) {
    BallSaveEndTime = 0;
    NumberOfBallSavesRemaining = 0;
  }

  if (!ScrollingScores && CurrentScores[CurrentPlayer] > RPU_OS_MAX_DISPLAY_SCORE) {
    CurrentScores[CurrentPlayer] -= RPU_OS_MAX_DISPLAY_SCORE;
    if (!TournamentScoring) AddSpecialCredit();
  }

  if (scoreAtTop != CurrentScores[CurrentPlayer]) {
    Display_SetLastTimeScoreChanged(CurrentTime);
    if (!TournamentScoring) {
      for (int awardCount = 0; awardCount < 3; awardCount++) {
        if (AwardScores[awardCount] != 0 && scoreAtTop < AwardScores[awardCount] && CurrentScores[CurrentPlayer] >= AwardScores[awardCount]) {
          // Player has just passed an award score, so we need to award it
          if (((ScoreAwardReplay >> awardCount) & 0x01)) {
            AddSpecialCredit();
          } else {
            AwardExtraBall();
          }
        }
      }
    }

  }

  return returnState;
}


#if (RPU_MPU_ARCHITECTURE>=10)
unsigned long LastLEDUpdateTime = 0;
byte LEDPhase = 0;
#endif

#ifdef DEBUG_SHOW_LOOPS_PER_SECOND
unsigned long NumLoops = 0;
unsigned long LastLoopReportTime = 0;
#endif

void loop() {

  CurrentTime = millis();
  int newMachineState = MachineState;
  
#ifdef DEBUG_SHOW_LOOPS_PER_SECOND
  NumLoops += 1;
  if (LastLoopReportTime==0) LastLoopReportTime = CurrentTime;
  if (CurrentTime>(LastLoopReportTime+1000)) {
    LastLoopReportTime = CurrentTime;
    char buf[128];
    sprintf(buf, "Loop running at %lu Hz\n", NumLoops);
    Serial.write(buf);
    NumLoops = 0;
  }
#endif

  if (Menus.OperatorMenusActive()) {
    RunOperatorMenu();
  } else {
    if (RestoreBackgroundTrack!=BACKGROUND_TRACK_NONE) {
      PlayBackgroundSong(RestoreBackgroundTrack);
      RestoreBackgroundTrack = BACKGROUND_TRACK_NONE;
    }
    if (MachineState < 0) {
      newMachineState = 0;
    } else if (MachineState == MACHINE_STATE_ATTRACT) {
      newMachineState = RunAttractMode(MachineState, MachineStateChanged);
    } else if (MachineState == MACHINE_STATE_DIAGNOSTICS) {
      newMachineState = RunDiagnosticsMode(MachineState, MachineStateChanged);
    } else {
      newMachineState = RunGamePlayMode(MachineState, MachineStateChanged);
    }  
  }
  
  if (newMachineState != MachineState) {
    MachineState = newMachineState;
    MachineStateChanged = true;
  } else {
    MachineStateChanged = false;
  }

  RPU_Update(CurrentTime);
  Audio.Update(CurrentTime);

#ifdef RPU_OS_USE_PINNODE_LINK
  // PinNode: run the link (a frame every 5 ms; keep every pass of loop() well under 100 ms),
  // report the mode for the app, and service HELLO, settings, audits and app requests.
  RPULink_Update(CurrentTime);
  {
    boolean inGame = (MachineState > MACHINE_STATE_ATTRACT && MachineState != MACHINE_STATE_DIAGNOSTICS);
    byte mode = (Menus.OperatorMenusActive() || MachineState == MACHINE_STATE_DIAGNOSTICS) ? PINNODE_MODE_SERVICE
              : inGame ? PINNODE_MODE_GAME : PINNODE_MODE_ATTRACT;
    // (during the match sequence the ball number has already moved past the last ball)
    PinNodeCtrl_Mode(mode, inGame ? ((CurrentBallInPlay > BallsPerGame) ? BallsPerGame : CurrentBallInPlay) : 0, inGame ? CurrentPlayer + 1 : 0);
  }
  // Live scores for phones at the machine (during a game: on a change, at most 4 a second)
  PinNodeCtrl_Scores(CurrentScores, CurrentNumPlayers);
  // Credits added from the app (the owner's "Add credit"): in attract only, the game's own
  // way (saved, shown, coin lockout), and never past the maximum.
  uint16_t creditRequest;
  byte creditsAsked;
  if (PinNodeCtrl_TakeCreditAdd(&creditRequest, &creditsAsked)) {
    if (MachineState != MACHINE_STATE_ATTRACT) {
      PinNodeCtrl_CreditResult(creditRequest, PINNODE_CREDIT_NOT_NOW, 0, Credits);
    } else if (creditsAsked == 0 || (unsigned int)Credits + creditsAsked > MaximumCredits) {
      PinNodeCtrl_CreditResult(creditRequest, PINNODE_CREDIT_OVER_MAX, 0, Credits);
    } else {
      AddCredit(false, creditsAsked);
      PinNodeCtrl_CreditResult(creditRequest, PINNODE_CREDIT_OK, creditsAsked, Credits);
    }
  }
  // the machine's credits, for the app: shown there, and no Start button at none
  PinNodeCtrl_Credits(Credits, FreePlayMode);
  PinNodeCtrl_Update(CurrentTime, false);   // no Mega firmware update over the WiFi socket
#endif

#if (RPU_MPU_ARCHITECTURE>=10)
  if (LastLEDUpdateTime == 0 || (CurrentTime - LastLEDUpdateTime) > 250) {
    LastLEDUpdateTime = CurrentTime;
    RPU_SetBoardLEDs((LEDPhase % 8) == 1 || (LEDPhase % 8) == 3, (LEDPhase % 8) == 5 || (LEDPhase % 8) == 7);
    LEDPhase += 1;
  }
#endif

}
