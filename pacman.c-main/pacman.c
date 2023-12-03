/*------------------------------------------------------------------------------
    pacman.c
    Test 1 Tommy Pham 
    Test 2 Tommy Pham
    Test 3
    test 4
    Test 5 Tommy Pham 
    test 6
    test 7 mike
    

    testimonial 18
    A Pacman clone written in C99 using the sokol headers for platform
    abstraction.

    The git repository is here:

    https://github.com/floooh/pacman.c

    A WASM version running in browsers can be found here:

    https://floooh.github.io/pacman.c/pacman.html

    Some basic concepts and ideas are worth explaining upfront:

    The game code structure is a bit "radical" and sometimes goes against
    what is considered good practice for medium and large code bases. This is
    fine because this is a small game written by a single person. Small
    code bases written by small teams allow a different organizational
    approach than large code bases written by large teams.

    Here are some of those "extremist" methods used in this tiny project:

    Instead of artificially splitting the code into many small source files,
    everything is in a single source file readable from top to bottom.

    Instead of data-encapsulation and -hiding, all data lives in a single,
    global, nested data structure (this isn't actually as radical and
    experimental as it sounds, I've been using this approach for quite a
    while now for all my hobby code). An interesting side effect of this
    upfront-defined static memory layout is that there are no dynamic
    allocations in the entire game code (only a handful allocations during
    initialization of the Sokol headers).

    Instead of "wasting" time thinking too much about high-level abstractions
    and reusability, the code has been written in a fairly adhoc-manner "from
    start to finish", solving problems as they showed up in the most direct
    way possible. When parts of the code became too entangled I tried to step
    back a bit, take a pause and come back later with a better idea how to
    rewrite those parts in a more straightforward manner. Of course
    "straightforward" and "readability" are in the eye of the beholder.

    The actual gameplay code (Pacman and ghost behaviours) has been
    implemented after the "Pacman Dossier" by Jamey Pittman (a PDF copy has
    been included in the project), but there are some minor differences to a
    Pacman arcade machine emulator, some intended, some not
    (https://floooh.github.io/tiny8bit/pacman.html):

        - The attract mode animation in the intro screen is missing (where
          Pacman is chased by ghosts, eats a pill and then hunts the ghost).
        - Likewise, the 'interlude' animation between levels is missing.
        - Various difficulty-related differences in later maps are ignored
          (such a faster movement speed, smaller dot-counter-limits for ghosts etc)

    The rendering and audio code resembles the original Pacman arcade machine
    hardware:

        - the tile and sprite pixel data, hardware color palette data and
          sound wavetable data is taken directly from embedded arcade machine
          ROM dumps
        - background tiles are rendered from two 28x36 byte buffers (one for
          tile-codes, the other for color-codes), this is similar to an actual
          arcade machine, with the only difference that the tile- and color-buffer
          has a straightforward linear memory layout
        - background tile rendering is done with dynamically uploaded vertex
          data (two triangles per tile), with color-palette decoding done in
          the pixel shader
        - up to 8 16x16 sprites are rendered as vertex quads, with the same
          color palette decoding happening in the pixel shader as for background
          tiles.
        - audio output works through an actual Namco WSG emulator which generates
          sound samples for 3 hardware voices from a 20-bit frequency counter,
          4-bit volume and 3-bit wave type (for 8 wavetables made of 32 sample
          values each stored in a ROM dump)
        - sound effects are implemented by writing new values to the hardware
          voice 'registers' once per 60Hz tick, this can happen in two ways:
            - as 'procedural' sound effects, where a callback function computes
              the new voice register values
            - or via 'register dump' playback, where the voice register values
              have been captured at 60Hz frequency from an actual Pacman arcade
              emulator
          Only two sound effects are register dumps: the little music track at
          the start of a game, and the sound effect when Pacman dies. All other
          effects are simple procedural effects.

    The only concept worth explaining in the gameplay code is how timing
    and 'asynchronous actions' work:

    The entire gameplay logic is driven by a global 60 Hz game tick which is
    counting upward.

    Gameplay actions are initiated by a combination of 'time triggers' and a simple
    vocabulary to initialize and test trigger conditions. This time trigger system
    is an extremely simple replacement for more powerful event systems in
    'proper' game engines.

    Here are some pseudo-code examples how time triggers can be used (unrelated
    to Pacman):

    To immediately trigger an action in one place of the code, and 'realize'
    this action in one or several other places:

        // if a monster has been eaten, trigger the 'monster eaten' action:
        if (monster_eaten()) {
            start(&state.game.monster_eaten);
        }

        // ...somewhere else, we might increase the score if a monster has been eaten:
        if (now(state.game.monster_eaten)) {
            state.game.score += 10;
        }

        // ...and yet somewhere else in the code, we might want to play a sound effect
        if (now(state.game.monster_eaten)) {
            // play sound effect...
        }

    We can also start actions in the future, which allows to batch multiple
    followup-actions in one place:

        // start fading out now, after one second (60 ticks) start a new
        // game round, and fade in, after another second when fadein has
        // finished, start the actual game loop
        start(&state.gfx.fadeout);
        start_after(&state.game.started, 60);
        start_after(&state.gfx.fadein, 60);
        start_after(&state.game.gameloop_started, 2*60);

    As mentioned above, there's a whole little function vocabulary built around
    time triggers, but those are hopefully all self-explanatory.
*/
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_audio.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#include <assert.h>
#include <string.h> // memset()
#include <stdlib.h> // abs()

// config defines and global constants
#define AUDIO_VOLUME (0.5f)
#define DBG_SKIP_INTRO      (0)     // set to (1) to skip intro
#define DBG_SKIP_PRELUDE    (0)     // set to (1) to skip game prelude
#define DBG_START_ROUND     (0)     // set to any starting round <=255
#define DBG_MARKERS         (0)     // set to (1) to show debug markers
#define DBG_ESCAPE          (1)     // set to (1) to leave game loop with Esc
#define DBG_DOUBLE_SPEED    (0)     // set to (1) to speed up game (useful with godmode)
#define DBG_GODMODE         (0)     // set to (1) to disable dying

// NOTE: DO NOT CHANGE THESE DEFINES TO AN ENUM
// gcc-13 will turn the backing type into an unsigned integer which then
// causes all sorts of trouble further down

// tick duration in nanoseconds
#if DBG_DOUBLE_SPEED
    #define TICK_DURATION_NS (8333333)
#else
    #define TICK_DURATION_NS (16666666)
#endif
#define TICK_TOLERANCE_NS    (1000000)      // per-frame tolerance in nanoseconds
#define NUM_VOICES           (3)            // number of sound voices
#define NUM_SOUNDS           (3)            // max number of sounds effects that can be active at a time
#define NUM_SAMPLES          (128)          // max number of audio samples in local sample buffer
#define DISABLED_TICKS       (0xFFFFFFFF)   // magic tick value for a disabled timer
#define TILE_WIDTH           (8)            // width and height of a background tile in pixels
#define TILE_HEIGHT          (8)
#define SPRITE_WIDTH         (16)           // width and height of a sprite in pixels
#define SPRITE_HEIGHT        (16)
#define DISPLAY_TILES_X      (28)           // tile buffer width and height
#define DISPLAY_TILES_Y      (36)
#define DISPLAY_PIXELS_X     (DISPLAY_TILES_X * TILE_WIDTH)
#define DISPLAY_PIXELS_Y     (DISPLAY_TILES_Y * TILE_HEIGHT)
#define NUM_SPRITES          (8)
#define NUM_DEBUG_MARKERS    (16)
#define TILE_TEXTURE_WIDTH   (256 * TILE_WIDTH)
#define TILE_TEXTURE_HEIGHT  (TILE_HEIGHT + SPRITE_HEIGHT)
#define MAX_VERTICES         (((DISPLAY_TILES_X * DISPLAY_TILES_Y) + NUM_SPRITES + NUM_DEBUG_MARKERS) * 6)
#define FADE_TICKS           (30)   // duration of fade-in/out
#define NUM_LIVES            (6)
#define NUM_STATUS_FRUITS    (7)    // max number of displayed fruits at bottom right
#define NUM_DOTS             (244)  // 240 small dots + 4 pills
#define NUM_PILLS            (4)    // number of energizer pills on playfield
#define ANTEPORTAS_X         (14*TILE_WIDTH)  // pixel position of the ghost house enter/leave point
#define ANTEPORTAS_Y         (14*TILE_HEIGHT + TILE_HEIGHT/2)
#define GHOST_EATEN_FREEZE_TICKS (60)  // number of ticks the game freezes after Pacman eats a ghost
#define PACMAN_EATEN_TICKS   (60)       // number of ticks to freeze game when Pacman is eaten
#define PACMAN_DEATH_TICKS   (150)      // number of ticks to show the Pacman death sequence before starting new round
#define GAMEOVER_TICKS       (3*60)     // number of ticks the game over message is shown
#define ROUNDWON_TICKS       (4*60)     // number of ticks to wait after a round was won
#define FRUITACTIVE_TICKS    (10*60)    // number of ticks a bonus fruit is shown

/* common tile, sprite and color codes, these are the same as on the Pacman
   arcade machine and extracted by looking at memory locations of a Pacman emulator
*/
enum {
    TILE_SPACE          = 0x40,
    TILE_DOT            = 0x10,
    TILE_PILL           = 0x14,
    TILE_GHOST          = 0xB0,
    TILE_LIFE           = 0x20, // 0x20..0x23
    TILE_CHERRIES       = 0x90, // 0x90..0x93
    TILE_STRAWBERRY     = 0x94, // 0x94..0x97
    TILE_PEACH          = 0x98, // 0x98..0x9B
    TILE_BELL           = 0x9C, // 0x9C..0x9F
    TILE_APPLE          = 0xA0, // 0xA0..0xA3
    TILE_GRAPES         = 0xA4, // 0xA4..0xA7
    TILE_GALAXIAN       = 0xA8, // 0xA8..0xAB
    TILE_KEY            = 0xAC, // 0xAC..0xAF
    TILE_DOOR           = 0xCF, // the ghost-house door

    SPRITETILE_INVISIBLE    = 30,
    SPRITETILE_SCORE_200    = 40,
    SPRITETILE_SCORE_400    = 41,
    SPRITETILE_SCORE_800    = 42,
    SPRITETILE_SCORE_1600   = 43,
    SPRITETILE_CHERRIES     = 0,
    SPRITETILE_STRAWBERRY   = 1,
    SPRITETILE_PEACH        = 2,
    SPRITETILE_BELL         = 3,
    SPRITETILE_APPLE        = 4,
    SPRITETILE_GRAPES       = 5,
    SPRITETILE_GALAXIAN     = 6,
    SPRITETILE_KEY          = 7,
    SPRITETILE_PACMAN_CLOSED_MOUTH = 48,

    COLOR_BLANK         = 0x00,
    COLOR_DEFAULT       = 0x0F,
    COLOR_DOT           = 0x10,
    COLOR_PACMAN        = 0x09,
    COLOR_BLINKY        = 0x01,
    COLOR_PINKY         = 0x03,
    COLOR_INKY          = 0x05,
    COLOR_CLYDE         = 0x07,
    COLOR_FRIGHTENED    = 0x11,
    COLOR_FRIGHTENED_BLINKING = 0x12,
    COLOR_GHOST_SCORE   = 0x18,
    COLOR_EYES          = 0x19,
    COLOR_CHERRIES      = 0x14,
    COLOR_STRAWBERRY    = 0x0F,
    COLOR_PEACH         = 0x15,
    COLOR_BELL          = 0x16,
    COLOR_APPLE         = 0x14,
    COLOR_GRAPES        = 0x17,
    COLOR_GALAXIAN      = 0x09,
    COLOR_KEY           = 0x16,
    COLOR_WHITE_BORDER  = 0x1F,
    COLOR_FRUIT_SCORE   = 0x03,
};

// the top-level game states (intro => game => intro)
typedef enum {
    GAMESTATE_INTRO,
    GAMESTATE_GAME,
} gamestate_t;

// directions NOTE: bit0==0: horizontal movement, bit0==1: vertical movement
typedef enum {
    DIR_RIGHT,  // 000
    DIR_DOWN,   // 001
    DIR_LEFT,   // 010
    DIR_UP,     // 011
    NUM_DIRS
} dir_t;

// bonus fruit types
typedef enum {
    FRUIT_NONE,
    FRUIT_CHERRIES,
    FRUIT_STRAWBERRY,
    FRUIT_PEACH,
    FRUIT_APPLE,
    FRUIT_GRAPES,
    FRUIT_GALAXIAN,
    FRUIT_BELL,
    FRUIT_KEY,
    NUM_FRUITS
} fruit_t;

// sprite 'hardware' indices
typedef enum {
    SPRITE_PACMAN,
    SPRITE_BLINKY,
    SPRITE_PINKY,
    SPRITE_INKY,
    SPRITE_CLYDE,
    SPRITE_FRUIT,
} sprite_index_t;

// ghost types
typedef enum {
    GHOSTTYPE_BLINKY,
    GHOSTTYPE_PINKY,
    GHOSTTYPE_INKY,
    GHOSTTYPE_CLYDE,
    NUM_GHOSTS
} ghosttype_t;

// ghost AI states
typedef enum {
    GHOSTSTATE_NONE,
    GHOSTSTATE_CHASE,           // currently chasing Pacman
    GHOSTSTATE_SCATTER,         // currently heading to the corner scatter targets
    GHOSTSTATE_FRIGHTENED,      // frightened after Pacman has eaten an energizer pill
    GHOSTSTATE_EYES,            // eaten by Pacman and heading back to the ghost house
    GHOSTSTATE_HOUSE,           // currently inside the ghost house
    GHOSTSTATE_LEAVEHOUSE,      // currently leaving the ghost house
    GHOSTSTATE_ENTERHOUSE       // currently entering the ghost house
} ghoststate_t;

// reasons why game loop is frozen
typedef enum {
    FREEZETYPE_PRELUDE   = (1<<0),  // game prelude is active (with the game start tune playing)
    FREEZETYPE_READY     = (1<<1),  // READY! phase is active (at start of a new game round)
    FREEZETYPE_EAT_GHOST = (1<<2),  // Pacman has eaten a ghost
    FREEZETYPE_DEAD      = (1<<3),  // Pacman was eaten by a ghost
    FREEZETYPE_WON       = (1<<4),  // game round was won by eating all dots
} freezetype_t;

// a trigger holds a specific game-tick when an action should be started
typedef struct {
    uint32_t tick;
} trigger_t;

// a 2D integer vector (used both for pixel- and tile-coordinates)
typedef struct {
    int16_t x;
    int16_t y;
} int2_t;

// common state for pacman and ghosts
typedef struct {
    dir_t dir;              // current movement direction
    int2_t pos;             // position of sprite center in pixel coords
    uint32_t anim_tick;     // incremented when actor moved in current tick
} actor_t;

// ghost AI state
typedef struct {
    actor_t actor;
    ghosttype_t type;
    dir_t next_dir;         // ghost AI looks ahead one tile when deciding movement direction
    int2_t target_pos;      // current target position in tile coordinates
    ghoststate_t state;
    trigger_t frightened;   // game tick when frightened mode was entered
    trigger_t eaten;        // game tick when eaten by Pacman
    uint16_t dot_counter;   // used to decide when to leave the ghost house
    uint16_t dot_limit;
} ghost_t;

// pacman state
typedef struct {
    actor_t actor;
} pacman_t;

// the tile- and sprite-renderer's vertex structure
typedef struct {
    float x, y;         // screen coords [0..1]
    float u, v;         // tile texture coords
    uint32_t attr;      // x: color code, y: opacity (opacity only used for fade effect)
} vertex_t;

// sprite state
typedef struct {
    bool enabled;           // if false sprite is deactivated
    uint8_t tile, color;    // sprite-tile number (0..63), color code
    bool flipx, flipy;      // horizontal/vertical flip
    int2_t pos;             // pixel position of the sprite's top-left corner
} sprite_t;

// debug visualization markers (see DBG_MARKERS)
typedef struct {
    bool enabled;
    uint8_t tile, color;    // tile and color code
    int2_t tile_pos;
} debugmarker_t;

// callback function prototype for procedural sounds
typedef void (*sound_func_t)(int sound_slot);

// a sound effect description used as param for snd_start()
typedef struct {
    sound_func_t func;      // callback function (if procedural sound)
    const uint32_t* ptr;    // pointer to register dump data (if a register-dump sound)
    uint32_t size;          // byte size of register dump data
    bool voice[3];          // true to activate voice
} sound_desc_t;

// a sound 'hardware' voice
typedef struct {
    uint32_t counter;   // 20-bit counter, top 5 bits are index into wavetable ROM
    uint32_t frequency; // 20-bit frequency (added to counter at 96kHz)
    uint8_t waveform;   // 3-bit waveform index
    uint8_t volume;     // 4-bit volume
    float sample_acc;   // current float sample accumulator
    float sample_div;   // current float sample divisor
} voice_t;

// flags for sound_t.flags
typedef enum {
    SOUNDFLAG_VOICE0 = (1<<0),
    SOUNDFLAG_VOICE1 = (1<<1),
    SOUNDFLAG_VOICE2 = (1<<2),
    SOUNDFLAG_ALL_VOICES = (1<<0)|(1<<1)|(1<<2)
} soundflag_t;

// a currently playing sound effect
typedef struct {
    uint32_t cur_tick;      // current tick counter
    sound_func_t func;      // optional function pointer for prodecural sounds
    uint32_t num_ticks;     // length of register dump sound effect in 60Hz ticks
    uint32_t stride;        // number of uint32_t values per tick (only for register dump effects)
    const uint32_t* data;   // 3 * num_ticks register dump values
    uint8_t flags;          // combination of soundflag_t (active voices)
} sound_t;

// all state is in a single nested struct
static struct {

    gamestate_t gamestate;  // the current gamestate (intro => game => intro)

    struct {
        uint32_t tick;          // the central game tick, this drives the whole game
        uint64_t laptime_store; // helper variable to measure frame duration
        int32_t tick_accum;     // helper variable to decouple ticks from frame rate
    } timing;

    // intro state
    struct {
        trigger_t started;      // tick when intro-state was started
    } intro;

    // game state
    struct {
        uint32_t xorshift;          // current xorshift random-number-generator state
        uint32_t hiscore;           // hiscore / 10
        trigger_t started;
        trigger_t ready_started;
        trigger_t round_started;
        trigger_t round_won;
        trigger_t game_over;
        trigger_t dot_eaten;            // last time Pacman ate a dot
        trigger_t pill_eaten;           // last time Pacman ate a pill
        trigger_t ghost_eaten;          // last time Pacman ate a ghost
        trigger_t pacman_eaten;         // last time Pacman was eaten by a ghost
        trigger_t fruit_eaten;          // last time Pacman has eaten the bonus fruit
        trigger_t force_leave_house;    // starts when a dot is eaten
        trigger_t fruit_active;         // starts when bonus fruit is shown
        uint8_t freeze;                 // combination of FREEZETYPE_* flags
        uint8_t round;                  // current game round, 0, 1, 2...
        uint32_t score;                 // score / 10
        int8_t num_lives;
        uint8_t num_ghosts_eaten;       // number of ghosts easten with current pill
        uint8_t num_dots_eaten;         // if == NUM_DOTS, Pacman wins the round
        bool global_dot_counter_active;     // set to true when Pacman loses a life
        uint8_t global_dot_counter;         // the global dot counter for the ghost-house-logic
        ghost_t ghost[NUM_GHOSTS];
        pacman_t pacman1;
        pacman_t pacman2;
        bool player2;
        fruit_t active_fruit;
    } game;

    // the current input state
    struct {
        bool enabled;
        bool up;
        bool down;
        bool left;
        bool right;
        bool esc;       // only for debugging (see DBG_ESCACPE)
        bool anykey;
    } input1;

    struct {
        bool enabled;
        bool up;
        bool down;
        bool left;
        bool right;
        bool esc;       // only for debugging (see DBG_ESCACPE)
        bool anykey;
    } input2;


    // the audio subsystem is essentially a Namco arcade board sound emulator
    struct {
        voice_t voice[NUM_VOICES];
        sound_t sound[NUM_SOUNDS];
        int32_t voice_tick_accum;
        int32_t voice_tick_period;
        int32_t sample_duration_ns;
        int32_t sample_accum;
        uint32_t num_samples;
        float sample_buffer[NUM_SAMPLES];
    } audio;

    // the gfx subsystem implements a simple tile+sprite renderer
    struct {
        // fade-in/out timers and current value
        trigger_t fadein;
        trigger_t fadeout;
        uint8_t fade;

        // the 36x28 tile framebuffer
        uint8_t video_ram[DISPLAY_TILES_Y][DISPLAY_TILES_X]; // tile codes
        uint8_t color_ram[DISPLAY_TILES_Y][DISPLAY_TILES_X]; // color codes

        // up to 8 sprites
        sprite_t sprite[NUM_SPRITES];

        // up to 16 debug markers
        debugmarker_t debug_marker[NUM_DEBUG_MARKERS];

        // sokol-gfx resources
        sg_pass_action pass_action;
        struct {
            sg_buffer vbuf;
            sg_image tile_img;
            sg_image palette_img;
            sg_image render_target;
            sg_sampler sampler;
            sg_pipeline pip;
            sg_pass pass;
        } offscreen;
        struct {
            sg_buffer quad_vbuf;
            sg_pipeline pip;
            sg_sampler sampler;
        } display;

        // intermediate vertex buffer for tile- and sprite-rendering
        int num_vertices;
        vertex_t vertices[MAX_VERTICES];

        // scratch-buffer for tile-decoding (only happens once)
        uint8_t tile_pixels[TILE_TEXTURE_HEIGHT][TILE_TEXTURE_WIDTH];

        // scratch buffer for the color palette
        uint32_t color_palette[256];
    } gfx;
} state;

// scatter target positions (in tile coords)
static const int2_t ghost_scatter_targets[NUM_GHOSTS] = {
    { 25, 0 }, { 2, 0 }, { 27, 34 }, { 0, 34 }
};

// starting positions for ghosts (pixel coords)
static const int2_t ghost_starting_pos[NUM_GHOSTS] = {
    { 14*8, 14*8 + 4 },
    { 14*8, 17*8 + 4 },
    { 12*8, 17*8 + 4 },
    { 16*8, 17*8 + 4 },
};

// target positions for ghost entering the ghost house (pixel coords)
static const int2_t ghost_house_target_pos[NUM_GHOSTS] = {
    { 14*8, 17*8 + 4 },
    { 14*8, 17*8 + 4 },
    { 12*8, 17*8 + 4 },
    { 16*8, 17*8 + 4 },
};

// fruit tiles, sprite tiles and colors
static const uint8_t fruit_tiles_colors[NUM_FRUITS][3] = {
    { 0, 0, 0 },   // FRUIT_NONE
    { TILE_CHERRIES,    SPRITETILE_CHERRIES,    COLOR_CHERRIES },
    { TILE_STRAWBERRY,  SPRITETILE_STRAWBERRY,  COLOR_STRAWBERRY },
    { TILE_PEACH,       SPRITETILE_PEACH,       COLOR_PEACH },
    { TILE_APPLE,       SPRITETILE_APPLE,       COLOR_APPLE },
    { TILE_GRAPES,      SPRITETILE_GRAPES,      COLOR_GRAPES },
    { TILE_GALAXIAN,    SPRITETILE_GALAXIAN,    COLOR_GALAXIAN },
    { TILE_BELL,        SPRITETILE_BELL,        COLOR_BELL },
    { TILE_KEY,         SPRITETILE_KEY,         COLOR_KEY }
};

// the tiles for displaying the bonus-fruit-score, this is a number built from 4 tiles
static const uint8_t fruit_score_tiles[NUM_FRUITS][4] = {
    { 0x40, 0x40, 0x40, 0x40 }, // FRUIT_NONE
    { 0x40, 0x81, 0x85, 0x40 }, // FRUIT_CHERRIES: 100
    { 0x40, 0x82, 0x85, 0x40 }, // FRUIT_STRAWBERRY: 300
    { 0x40, 0x83, 0x85, 0x40 }, // FRUIT_PEACH: 500
    { 0x40, 0x84, 0x85, 0x40 }, // FRUIT_APPLE: 700
    { 0x40, 0x86, 0x8D, 0x8E }, // FRUIT_GRAPES: 1000
    { 0x87, 0x88, 0x8D, 0x8E }, // FRUIT_GALAXIAN: 2000
    { 0x89, 0x8A, 0x8D, 0x8E }, // FRUIT_BELL: 3000
    { 0x8B, 0x8C, 0x8D, 0x8E }, // FRUIT_KEY: 5000
};

// level specifications (see pacman_dossier.pdf)
typedef struct {
    fruit_t bonus_fruit;
    uint16_t bonus_score;
    uint16_t fright_ticks;
    // FIXME: the various Pacman and ghost speeds
} levelspec_t;

enum {
    MAX_LEVELSPEC = 21,
};
static const levelspec_t levelspec_table[MAX_LEVELSPEC] = {
    { FRUIT_CHERRIES,   10,  6*60, },
    { FRUIT_STRAWBERRY, 30,  5*60, },
    { FRUIT_PEACH,      50,  4*60, },
    { FRUIT_PEACH,      50,  3*60, },
    { FRUIT_APPLE,      70,  2*60, },
    { FRUIT_APPLE,      70,  5*60, },
    { FRUIT_GRAPES,     100, 2*60, },
    { FRUIT_GRAPES,     100, 2*60, },
    { FRUIT_GALAXIAN,   200, 1*60, },
    { FRUIT_GALAXIAN,   200, 5*60, },
    { FRUIT_BELL,       300, 2*60, },
    { FRUIT_BELL,       300, 1*60, },
    { FRUIT_KEY,        500, 1*60, },
    { FRUIT_KEY,        500, 3*60, },
    { FRUIT_KEY,        500, 1*60, },
    { FRUIT_KEY,        500, 1*60, },
    { FRUIT_KEY,        500, 1,    },
    { FRUIT_KEY,        500, 1*60, },
    { FRUIT_KEY,        500, 1,    },
    { FRUIT_KEY,        500, 1,    },
    { FRUIT_KEY,        500, 1,    },
    // from here on repeating
};

// forward-declared sound-effect register dumps (recorded from Pacman arcade emulator)
static const uint32_t snd_dump_prelude[490];
static const uint32_t snd_dump_dead[90];

// procedural sound effect callbacks
static void snd_func_eatdot1(int slot);
static void snd_func_eatdot2(int slot);
static void snd_func_eatghost(int slot);
static void snd_func_eatfruit(int slot);
static void snd_func_weeooh(int slot);
static void snd_func_frightened(int slot);

// sound effect description structs
static const sound_desc_t snd_prelude = {
    .ptr = snd_dump_prelude,
    .size = sizeof(snd_dump_prelude),
    .voice = { true, true, false }
};

static const sound_desc_t snd_dead = {
    .ptr = snd_dump_dead,
    .size = sizeof(snd_dump_dead),
    .voice = { false, false, true }
};

static const sound_desc_t snd_eatdot1 = {
    .func = snd_func_eatdot1,
    .voice = { false, false, true }
};

static const sound_desc_t snd_eatdot2 = {
    .func = snd_func_eatdot2,
    .voice = { false, false, true }
};

static const sound_desc_t snd_eatghost = {
    .func = snd_func_eatghost,
    .voice = { false, false, true }
};

static const sound_desc_t snd_eatfruit = {
    .func = snd_func_eatfruit,
    .voice = { false, false, true }
};

static const sound_desc_t snd_weeooh = {
    .func = snd_func_weeooh,
    .voice = { false, true, false }
};

static const sound_desc_t snd_frightened = {
    .func = snd_func_frightened,
    .voice = { false, true, false }
};

// forward declarations
static void init(void);
static void frame(void);
static void cleanup(void);
static void input(const sapp_event*);
static void input2(const sapp_event*);


static void start(trigger_t* t);
static bool now(trigger_t t);

static void intro_tick(void);
static void game_tick(void);

static void input_enable(void);
static void input_disable(void);

static void gfx_init(void);
static void gfx_shutdown(void);
static void gfx_fade(void);
static void gfx_draw(void);

static void snd_init(void);
static void snd_shutdown(void);
static void snd_tick(void); // called per game tick
static void snd_frame(int32_t frame_time_ns);   // called per frame
static void snd_clear(void);
static void snd_start(int sound_slot, const sound_desc_t* snd);
static void snd_stop(int sound_slot);

// forward-declared ROM dumps
static const uint8_t rom_tiles[4096];
static const uint8_t rom_sprites[4096];
static const uint8_t rom_hwcolors[32];
static const uint8_t rom_palette[256];
static const uint8_t rom_wavetable[256];

/*== APPLICATION ENTRY AND CALLBACKS =========================================*/
sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    return (sapp_desc) {
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = input,
        .width = DISPLAY_TILES_X * TILE_WIDTH * 2,
        .height = DISPLAY_TILES_Y * TILE_HEIGHT * 2,
        .window_title = "Team-3_Pacman.c",
        .logger.func = slog_func,
    };
}

static void init(void) {
    gfx_init();
    snd_init();

    // start into intro screen
    #if DBG_SKIP_INTRO
        start(&state.game.started);
    #else
        start(&state.intro.started);
    #endif
}

static void frame(void) {

    // run the game at a fixed tick rate regardless of frame rate
    uint32_t frame_time_ns = (uint32_t) (sapp_frame_duration() * 1000000000.0);
    // clamp max frame time (so the timing isn't messed up when stopping in the debugger)
    if (frame_time_ns > 33333333) {
        frame_time_ns = 33333333;
    }
    state.timing.tick_accum += frame_time_ns;
    while (state.timing.tick_accum > -TICK_TOLERANCE_NS) {
        state.timing.tick_accum -= TICK_DURATION_NS;
        state.timing.tick++;

        // call per-tick sound function (updates sound 'registers' with current sound effect values)
        snd_tick();

        // check for game state change
        if (now(state.intro.started)) {
            state.gamestate = GAMESTATE_INTRO;
        }
        if (now(state.game.started)) {
            state.gamestate = GAMESTATE_GAME;
        }

        // call the top-level game state update function
        switch (state.gamestate) {
            case GAMESTATE_INTRO:
                intro_tick();
                break;
            case GAMESTATE_GAME:
                game_tick();
                break;
        }
    }
    gfx_draw();
    snd_frame(frame_time_ns);
}

static void input(const sapp_event* ev) {
    if (state.input1.enabled) {
        if ((ev->type == SAPP_EVENTTYPE_KEY_DOWN) || (ev->type == SAPP_EVENTTYPE_KEY_UP)) {
            bool btn_down = ev->type == SAPP_EVENTTYPE_KEY_DOWN;
            switch (ev->key_code) {
                case SAPP_KEYCODE_UP:
                    state.input1.up = state.input1.anykey = btn_down;
                    state.game.player2 = false;
                    break;
                case SAPP_KEYCODE_DOWN:
                    state.input1.down = state.input1.anykey = btn_down;
                    state.game.player2 = false;
                    break;
                case SAPP_KEYCODE_LEFT:
                    state.input1.left = state.input1.anykey = btn_down;
                    state.game.player2 = false;
                    break;
                case SAPP_KEYCODE_RIGHT:
                    state.input1.right = state.input1.anykey = btn_down;
                    state.game.player2 = false;
                    break;
                case SAPP_KEYCODE_ESCAPE:
                    state.input1.esc = state.input1.anykey = btn_down;
                    break;


                case SAPP_KEYCODE_W:
                    state.input2.up = state.input2.anykey = btn_down;
                    state.game.player2 = true;
                    break;
                case SAPP_KEYCODE_S:
                    state.input2.down = state.input2.anykey = btn_down;
                    state.game.player2 = true;

                    break;
                case SAPP_KEYCODE_A:
                    state.input2.left = state.input2.anykey = btn_down;
                    state.game.player2 = true;

                    break;
                case SAPP_KEYCODE_D:
                    state.input2.right = state.input2.anykey = btn_down;
                    state.game.player2 = true;

                    break;







                default:
                    state.input1.anykey = btn_down;
                    state.input2.anykey = btn_down;
                    break;
            }
        }
    }
}






static void cleanup(void) {
    snd_shutdown();
    gfx_shutdown();
}

/*== GRAB BAG OF HELPER FUNCTIONS ============================================*/

// xorshift random number generator
static uint32_t xorshift32(void) {
    uint32_t x = state.game.xorshift;
    x ^= x<<13;
    x ^= x>>17;
    x ^= x<<5;
    return state.game.xorshift = x;
}
// get level spec for a game round
static levelspec_t levelspec(int round) {
    assert(round >= 0);
    if (round >= MAX_LEVELSPEC) {
        round = MAX_LEVELSPEC-1;
    }
    return levelspec_table[round];
}

// set time trigger to the next game tick
static void start(trigger_t* t) {
    t->tick = state.timing.tick + 1;
}

// set time trigger to a future tick
static void start_after(trigger_t* t, uint32_t ticks) {
    t->tick = state.timing.tick + ticks;
}

// deactivate a time trigger
static void disable(trigger_t* t) {
    t->tick = DISABLED_TICKS;
}

// return a disabled time trigger
static trigger_t disabled_timer(void) {
    return (trigger_t) { .tick = DISABLED_TICKS };
}

// check if a time trigger is triggered
static bool now(trigger_t t) {
    return t.tick == state.timing.tick;
}

// return the number of ticks since a time trigger was triggered
static uint32_t since(trigger_t t) {
    if (state.timing.tick >= t.tick) {
        return state.timing.tick - t.tick;
    }
    else {
        return DISABLED_TICKS;
    }
}

// check if a time trigger is between begin and end tick
static bool between(trigger_t t, uint32_t begin, uint32_t end) {
    assert(begin < end);
    if (t.tick != DISABLED_TICKS) {
        uint32_t ticks = since(t);
        return (ticks >= begin) && (ticks < end);
    }
    else {
        return false;
    }
}

// check if a time trigger was triggered exactly N ticks ago
static bool after_once(trigger_t t, uint32_t ticks) {
    return since(t) == ticks;
}

// check if a time trigger was triggered more than N ticks ago
static bool after(trigger_t t, uint32_t ticks) {
    uint32_t s = since(t);
    if (s != DISABLED_TICKS) {
        return s >= ticks;
    }
    else {
        return false;
    }
}

// same as between(t, 0, ticks)
static bool before(trigger_t t, uint32_t ticks) {
    uint32_t s = since(t);
    if (s != DISABLED_TICKS) {
        return s < ticks;
    }
    else {
        return false;
    }
}

// clear input state and disable input
static void input_disable(void) {
    memset(&state.input1, 0, sizeof(state.input1));
    memset(&state.input2, 0, sizeof(state.input2));
}

// enable input again
static void input_enable(void) {
    state.input1.enabled = true;
    state.input2.enabled = true;
}

// get the current input as dir_t
static dir_t input_dir(dir_t default_dir) {
    if (state.input1.up) {
        return DIR_UP;
    }
    else if (state.input1.down) {
        return DIR_DOWN;
    }
    else if (state.input1.right) {
        return DIR_RIGHT;
    }
    else if (state.input1.left) {
        return DIR_LEFT;
    }
    else if (state.input2.up) {
        return DIR_UP;
    }
    else if (state.input2.down) {
        return DIR_DOWN;
    }
    else if (state.input2.right) {
        return DIR_RIGHT;
    }
    else if (state.input2.left) {
        return DIR_LEFT;
    }
    else {
        return default_dir;
    }
}

// shortcut to create an int2_t
static int2_t i2(int16_t x, int16_t y) {
    return (int2_t) { x, y };
}

// add two int2_t
static int2_t add_i2(int2_t v0, int2_t v1) {
    return (int2_t) { v0.x+v1.x, v0.y+v1.y };
}

// subtract two int2_t
static int2_t sub_i2(int2_t v0, int2_t v1) {
    return (int2_t) { v0.x-v1.x, v0.y-v1.y };
}

// multiply int2_t with scalar
static int2_t mul_i2(int2_t v, int16_t s) {
    return (int2_t) { v.x*s, v.y*s };
}

// squared-distance between two int2_t
static int32_t squared_distance_i2(int2_t v0, int2_t v1) {
    int2_t d = { v1.x - v0.x, v1.y - v0.y };
    return d.x * d.x + d.y * d.y;
}

// check if two int2_t are equal
static bool equal_i2(int2_t v0, int2_t v1) {
    return (v0.x == v1.x) && (v0.y == v1.y);
}

// check if two int2_t are nearly equal
static bool nearequal_i2(int2_t v0, int2_t v1, int16_t tolerance) {
    return (abs(v1.x - v0.x) <= tolerance) && (abs(v1.y - v0.y) <= tolerance);
}

// convert an actor pos (origin at center) to sprite pos (origin top left)
static int2_t actor_to_sprite_pos(int2_t pos) {
    return i2(pos.x - SPRITE_WIDTH/2, pos.y - SPRITE_HEIGHT/2);
}

// compute the distance of a pixel coordinate to the next tile midpoint
int2_t dist_to_tile_mid(int2_t pos) {
    return i2((TILE_WIDTH/2) - pos.x % TILE_WIDTH, (TILE_HEIGHT/2) - pos.y % TILE_HEIGHT);
}

// clear tile and color buffer
static void vid_clear(uint8_t tile_code, uint8_t color_code) {
    memset(&state.gfx.video_ram, tile_code, sizeof(state.gfx.video_ram));
    memset(&state.gfx.color_ram, color_code, sizeof(state.gfx.color_ram));
}

// clear the playfield's rectangle in the color buffer
static void vid_color_playfield(uint8_t color_code) {
    for (int y = 3; y < DISPLAY_TILES_Y-2; y++) {
        for (int x = 0; x < DISPLAY_TILES_X; x++) {
            state.gfx.color_ram[y][x] = color_code;
        }
    }
}

// check if a tile position is valid
static bool valid_tile_pos(int2_t tile_pos) {
    return ((tile_pos.x >= 0) && (tile_pos.x < DISPLAY_TILES_X) && (tile_pos.y >= 0) && (tile_pos.y < DISPLAY_TILES_Y));
}

// put a color into the color buffer
static void vid_color(int2_t tile_pos, uint8_t color_code) {
    assert(valid_tile_pos(tile_pos));
    state.gfx.color_ram[tile_pos.y][tile_pos.x] = color_code;
}

// put a tile into the tile buffer
static void vid_tile(int2_t tile_pos, uint8_t tile_code) {
    assert(valid_tile_pos(tile_pos));
    state.gfx.video_ram[tile_pos.y][tile_pos.x] = tile_code;
}

// put a colored tile into the tile and color buffers
static void vid_color_tile(int2_t tile_pos, uint8_t color_code, uint8_t tile_code) {
    assert(valid_tile_pos(tile_pos));
    state.gfx.video_ram[tile_pos.y][tile_pos.x] = tile_code;
    state.gfx.color_ram[tile_pos.y][tile_pos.x] = color_code;
}

// translate ASCII char into "NAMCO char"
static char conv_char(char c) {
    switch (c) {
        case ' ':   c = 0x40; break;
        case '/':   c = 58; break;
        case '-':   c = 59; break;
        case '\"':  c = 38; break;
        case '!':   c = 'Z'+1; break;
        default: break;
    }
    return c;
}

// put colored char into tile+color buffers
static void vid_color_char(int2_t tile_pos, uint8_t color_code, char chr) {
    assert(valid_tile_pos(tile_pos));
    state.gfx.video_ram[tile_pos.y][tile_pos.x] = conv_char(chr);
    state.gfx.color_ram[tile_pos.y][tile_pos.x] = color_code;
}

// put char into tile buffer
static void vid_char(int2_t tile_pos, char chr) {
    assert(valid_tile_pos(tile_pos));
    state.gfx.video_ram[tile_pos.y][tile_pos.x] = conv_char(chr);
}

// put colored text into the tile+color buffers
static void vid_color_text(int2_t tile_pos, uint8_t color_code, const char* text) {
    assert(valid_tile_pos(tile_pos));
    uint8_t chr;
    while ((chr = (uint8_t) *text++)) {
        if (tile_pos.x < DISPLAY_TILES_X) {
            vid_color_char(tile_pos, color_code, chr);
            tile_pos.x++;
        }
        else {
            break;
        }
    }
}

// put text into the tile buffer
static void vid_text(int2_t tile_pos, const char* text) {
    assert(valid_tile_pos(tile_pos));
    uint8_t chr;
    while ((chr = (uint8_t) *text++)) {
        if (tile_pos.x < DISPLAY_TILES_X) {
            vid_char(tile_pos, chr);
            tile_pos.x++;
        }
        else {
            break;
        }
    }
}

/* print colored score number into tile+color buffers from right to left(!),
    scores are /10, the last printed number is always 0,
    a zero-score will print as '00' (this is the same as on
    the Pacman arcade machine)
*/
static void vid_color_score(int2_t tile_pos, uint8_t color_code, uint32_t score) {
    vid_color_char(tile_pos, color_code, '0');
    tile_pos.x--;
    for (int digit = 0; digit < 8; digit++) {
        char chr = (score % 10) + '0';
        if (valid_tile_pos(tile_pos)) {
            vid_color_char(tile_pos, color_code, chr);
            tile_pos.x--;
            score /= 10;
            if (0 == score) {
                break;
            }
        }
    }
}

/* draw a colored tile-quad arranged as:
    |t+1|t+0|
    |t+3|t+2|

   This is (for instance) used to render the current "lives" and fruit
   symbols at the lower border.
*/
static void vid_draw_tile_quad(int2_t tile_pos, uint8_t color_code, uint8_t tile_code) {
    for (int yy=0; yy<2; yy++) {
        for (int xx=0; xx<2; xx++) {
            uint8_t t = tile_code + yy*2 + (1-xx);
            vid_color_tile(i2(xx + tile_pos.x, yy + tile_pos.y), color_code, t);
        }
    }
}

// draw the fruit bonus score tiles (when Pacman has eaten the bonus fruit)
static void vid_fruit_score(fruit_t fruit_type) {
    assert((fruit_type >= 0) && (fruit_type < NUM_FRUITS));
    uint8_t color_code = (fruit_type == FRUIT_NONE) ? COLOR_DOT : COLOR_FRUIT_SCORE;
    for (int i = 0; i < 4; i++) {
        vid_color_tile(i2(12+i, 20), color_code, fruit_score_tiles[fruit_type][i]);
        
    }
}

// disable and clear all sprites
static void spr_clear(void) {
    memset(&state.gfx.sprite, 0, sizeof(state.gfx.sprite));
}

// get pointer to pacman sprite
static sprite_t* spr_pacman(void) {
    return &state.gfx.sprite[SPRITE_PACMAN];
}

// get pointer to ghost sprite
static sprite_t* spr_ghost(ghosttype_t type) {
    assert((type >= 0) && (type < NUM_GHOSTS));
    return &state.gfx.sprite[SPRITE_BLINKY + type];
}

// get pointer to fruit sprite
static sprite_t* spr_fruit(void) {
    return &state.gfx.sprite[SPRITE_FRUIT];
}

// set sprite to animated Pacman
static void spr_anim_pacman(dir_t dir, uint32_t tick) {
    // animation frames for horizontal and vertical movement
    static const uint8_t tiles[2][4] = {
        { 44, 46, 48, 46 }, // horizontal (needs flipx)
        { 45, 47, 48, 47 }  // vertical (needs flipy)
    };
    sprite_t* spr = spr_pacman();
    uint32_t phase = (tick / 2) & 3;
    spr->tile  = tiles[dir & 1][phase];
    spr->color = COLOR_PACMAN;
    spr->flipx = (dir == DIR_LEFT);
    spr->flipy = (dir == DIR_UP);
}

// set sprite to Pacman's death sequence
static void spr_anim_pacman_death(uint32_t tick) {
    // the death animation tile sequence starts at sprite tile number 52 and ends at 63
    sprite_t* spr = spr_pacman();
    uint32_t tile = 52 + (tick / 8);
    if (tile > 63) {
        tile = 63;
    }
    spr->tile = tile;
    spr->flipx = spr->flipy = false;
}

// set sprite to animated ghost
static void spr_anim_ghost(ghosttype_t ghost_type, dir_t dir, uint32_t tick) {
    assert((dir >= 0) && (dir < NUM_DIRS));
    static const uint8_t tiles[4][2]  = {
        { 32, 33 }, // right
        { 34, 35 }, // down
        { 36, 37 }, // left
        { 38, 39 }, // up
    };
    uint32_t phase = (tick / 8) & 1;
    sprite_t* spr = spr_ghost(ghost_type);
    spr->tile = tiles[dir][phase];
    spr->color = COLOR_BLINKY + 2*ghost_type;
    spr->flipx = false;
    spr->flipy = false;
}

// set sprite to frightened ghost
static void spr_anim_ghost_frightened(ghosttype_t ghost_type, uint32_t tick) {
    static const uint8_t tiles[2] = { 28, 29 };
    uint32_t phase = (tick / 4) & 1;
    sprite_t* spr = spr_ghost(ghost_type);
    spr->tile = tiles[phase];
    if (tick > (uint32_t)(levelspec(state.game.round).fright_ticks - 60)) {
        // towards end of frightening period, start blinking
        spr->color = (tick & 0x10) ? COLOR_FRIGHTENED : COLOR_FRIGHTENED_BLINKING;
    }
    else {
        spr->color = COLOR_FRIGHTENED;
    }
    spr->flipx = false;
    spr->flipy = false;
}

/* set sprite to ghost eyes, these are the normal ghost sprite
    images but with a different color code which makes
    only the eyes visible
*/
static void spr_anim_ghost_eyes(ghosttype_t ghost_type, dir_t dir) {
    assert((dir >= 0) && (dir < NUM_DIRS));
    static const uint8_t tiles[NUM_DIRS] = { 32, 34, 36, 38 };
    sprite_t* spr = spr_ghost(ghost_type);
    spr->tile = tiles[dir];
    spr->color = COLOR_EYES;
    spr->flipx = false;
    spr->flipy = false;
}

// convert pixel position to tile position
static int2_t pixel_to_tile_pos(int2_t pix_pos) {
    return i2(pix_pos.x / TILE_WIDTH, pix_pos.y / TILE_HEIGHT);
}

// clamp tile pos to valid playfield coords
static int2_t clamped_tile_pos(int2_t tile_pos) {
    int2_t res = tile_pos;
    if (res.x < 0) {
        res.x = 0;
    }
    else if (res.x >= DISPLAY_TILES_X) {
        res.x = DISPLAY_TILES_X - 1;
    }
    if (res.y < 3) {
        res.y = 3;
    }
    else if (res.y >= (DISPLAY_TILES_Y-2)) {
        res.y = DISPLAY_TILES_Y - 3;
    }
    return res;
}

// convert a direction to a movement vector
static int2_t dir_to_vec(dir_t dir) {
    assert((dir >= 0) && (dir < NUM_DIRS));
    static const int2_t dir_map[NUM_DIRS] = { { +1, 0 }, { 0, +1 }, { -1, 0 }, { 0, -1 } };
    return dir_map[dir];
}

// return the reverse direction
static dir_t reverse_dir(dir_t dir) {
    switch (dir) {
        case DIR_RIGHT: return DIR_LEFT;
        case DIR_DOWN:  return DIR_UP;
        case DIR_LEFT:  return DIR_RIGHT;
        default:        return DIR_DOWN;
    }
}

// return tile code at tile position
static uint8_t tile_code_at(int2_t tile_pos) {
    assert((tile_pos.x >= 0) && (tile_pos.x < DISPLAY_TILES_X));
    assert((tile_pos.y >= 0) && (tile_pos.y < DISPLAY_TILES_Y));
    return state.gfx.video_ram[tile_pos.y][tile_pos.x];
}

// check if a tile position contains a blocking tile (walls and ghost house door)
static bool is_blocking_tile(int2_t tile_pos) {
    return tile_code_at(tile_pos) >= 0xC0;
}

// check if a tile position contains a dot tile
static bool is_dot(int2_t tile_pos) {
    return tile_code_at(tile_pos) == TILE_DOT;
}

// check if a tile position contains a pill tile
static bool is_pill(int2_t tile_pos) {
    return tile_code_at(tile_pos) == TILE_PILL;
}

// check if a tile position is in the teleport tunnel
static bool is_tunnel(int2_t tile_pos) {
    return (tile_pos.y == 17) && ((tile_pos.x <= 5) || (tile_pos.x >= 22));
}

// check if a position is in the ghost's red zone, where upward movement is forbidden
// (see Pacman Dossier "Areas To Exploit")
static bool is_redzone(int2_t tile_pos) {
    return ((tile_pos.x >= 11) && (tile_pos.x <= 16) && ((tile_pos.y == 14) || (tile_pos.y == 26)));
}

// test if movement from a pixel position in a wanted direction is possible,
// allow_cornering is Pacman's feature to take a diagonal shortcut around corners
static bool can_move(int2_t pos, dir_t wanted_dir, bool allow_cornering) {
    const int2_t dir_vec = dir_to_vec(wanted_dir);
    const int2_t dist_mid = dist_to_tile_mid(pos);

    // distance to midpoint in move direction and perpendicular direction
    int16_t move_dist_mid, perp_dist_mid;
    if (dir_vec.y != 0) {
        move_dist_mid = dist_mid.y;
        perp_dist_mid = dist_mid.x;
    }
    else {
        move_dist_mid = dist_mid.x;
        perp_dist_mid = dist_mid.y;
    }

    // look one tile ahead in movement direction
    const int2_t tile_pos = pixel_to_tile_pos(pos);
    const int2_t check_pos = clamped_tile_pos(add_i2(tile_pos, dir_vec));
    const bool is_blocked = is_blocking_tile(check_pos);
    if ((!allow_cornering && (0 != perp_dist_mid)) || (is_blocked && (0 == move_dist_mid))) {
        // way is blocked
        return false;
    }
    else {
        // way is free
        return true;
    }
}

// compute a new pixel position along a direction (without blocking check!)
static int2_t move(int2_t pos, dir_t dir, bool allow_cornering) {
    const int2_t dir_vec = dir_to_vec(dir);
    pos = add_i2(pos, dir_vec);

    // if cornering is allowed, drag the position towards the center-line
    if (allow_cornering) {
        const int2_t dist_mid = dist_to_tile_mid(pos);
        if (dir_vec.x != 0) {
            if (dist_mid.y < 0)      { pos.y--; }
            else if (dist_mid.y > 0) { pos.y++; }
        }
        else if (dir_vec.y != 0) {
            if (dist_mid.x < 0)      { pos.x--; }
            else if (dist_mid.x > 0) { pos.x++; }
        }
    }

    // wrap x-position around (only possible in the teleport-tunnel)
    if (pos.x < 0) {
        pos.x = DISPLAY_PIXELS_X - 1;
    }
    else if (pos.x >= DISPLAY_PIXELS_X) {
        pos.x = 0;
    }
    return pos;
}

// set a debug marker
#if DBG_MARKERS
static void dbg_marker(int index, int2_t tile_pos, uint8_t tile_code, uint8_t color_code) {
    assert((index >= 0) && (index < NUM_DEBUG_MARKERS));
    state.gfx.debug_marker[index] = (debugmarker_t) {
        .enabled = true,
        .tile = tile_code,
        .color = color_code,
        .tile_pos = clamped_tile_pos(tile_pos)
    };
}
#endif

/*== GAMEPLAY CODE ===========================================================*/

// initialize the playfield tiles
static void game_init_playfield(void) {
    vid_color_playfield(COLOR_DOT);
    // decode the playfield from an ASCII map into tiles codes
    static const char* tiles =
       //0123456789012345678901234567
        "0UUUUUUUUUUUU45UUUUUUUUUUUU1" // 3
        "L............rl............R" // 4
        "L.ebbf.ebbbf.rl.ebbbf.ebbf.R" // 5
        "LPr  l.r   l.rl.r   l.r  lPR" // 6
        "L.guuh.guuuh.gh.guuuh.guuh.R" // 7
        "L..........................R" // 8
        "L.ebbf.ef.ebbbbbbf.ef.ebbf.R" // 9
        "L.guuh.rl.guuyxuuh.rl.guuh.R" // 10
        "L......rl....rl....rl......R" // 11
        "2BBBBf.rzbbf rl ebbwl.eBBBB3" // 12
        "     L.rxuuh gh guuyl.R     " // 13
        "     L.rl          rl.R     " // 14
        "     L.rl mjs--tjn rl.R     " // 15
        "UUUUUh.gh i      q gh.gUUUUU" // 16
        "      .   i      q   .      " // 17
        "BBBBBf.ef i      q ef.eBBBBB" // 18
        "     L.rl okkkkkkp rl.R     " // 19
        "     L.rl          rl.R     " // 20
        "     L.rl ebbbbbbf rl.R     " // 21
        "0UUUUh.gh guuyxuuh gh.gUUUU1" // 22
        "L............rl............R" // 23
        "L.ebbf.ebbbf.rl.ebbbf.ebbf.R" // 24
        "L.guyl.guuuh.gh.guuuh.rxuh.R" // 25
        "LP..rl.......  .......rl..PR" // 26
        "6bf.rl.ef.ebbbbbbf.ef.rl.eb8" // 27
        "7uh.gh.rl.guuyxuuh.rl.gh.gu9" // 28
        "L......rl....rl....rl......R" // 29
        "L.ebbbbwzbbf.rl.ebbwzbbbbf.R" // 30
        "L.guuuuuuuuh.gh.guuuuuuuuh.R" // 31
        "L..........................R" // 32
        "2BBBBBBBBBBBBBBBBBBBBBBBBBB3"; // 33
       //0123456789012345678901234567
    uint8_t t[128];
    for (int i = 0; i < 128; i++) { t[i]=TILE_DOT; }
    t[' ']=0x40; t['0']=0xD1; t['1']=0xD0; t['2']=0xD5; t['3']=0xD4; t['4']=0xFB;
    t['5']=0xFA; t['6']=0xD7; t['7']=0xD9; t['8']=0xD6; t['9']=0xD8; t['U']=0xDB;
    t['L']=0xD3; t['R']=0xD2; t['B']=0xDC; t['b']=0xDF; t['e']=0xE7; t['f']=0xE6;
    t['g']=0xEB; t['h']=0xEA; t['l']=0xE8; t['r']=0xE9; t['u']=0xE5; t['w']=0xF5;
    t['x']=0xF2; t['y']=0xF3; t['z']=0xF4; t['m']=0xED; t['n']=0xEC; t['o']=0xEF;
    t['p']=0xEE; t['j']=0xDD; t['i']=0xD2; t['k']=0xDB; t['q']=0xD3; t['s']=0xF1;
    t['t']=0xF0; t['-']=TILE_DOOR; t['P']=TILE_PILL;
    for (int y = 3, i = 0; y <= 33; y++) {
        for (int x = 0; x < 28; x++, i++) {
            state.gfx.video_ram[y][x] = t[tiles[i] & 127];
        }
    }
    // ghost house gate colors
    vid_color(i2(13,15), 0x18);
    vid_color(i2(14,15), 0x18);
}

// disable all game loop timers
static void game_disable_timers(void) {
    disable(&state.game.round_won);
    disable(&state.game.game_over);
    disable(&state.game.dot_eaten);
    disable(&state.game.pill_eaten);
    disable(&state.game.ghost_eaten);
    disable(&state.game.pacman_eaten);
    disable(&state.game.fruit_eaten);
    disable(&state.game.force_leave_house);
    disable(&state.game.fruit_active);
}

// one-time init at start of game state
static void game_init(void) {
    input_enable();
    game_disable_timers();
    state.game.round = DBG_START_ROUND;
    state.game.freeze = FREEZETYPE_PRELUDE;
    state.game.num_lives = NUM_LIVES;
    state.game.global_dot_counter_active = false;
    state.game.global_dot_counter = 0;
    state.game.num_dots_eaten = 0;
    state.game.score = 0;

    // draw the playfield and PLAYER ONE READY! message
    vid_clear(TILE_SPACE, COLOR_DOT);
    vid_color_text(i2(9,0), COLOR_DEFAULT, "HIGH SCORE");
    game_init_playfield();
    vid_color_text(i2(9,14), 0x5, "PLAYER ONE");
    vid_color_text(i2(11, 20), 0x9, "READY!");
}

// setup state at start of a game round
static void game_round_init(void) {
    spr_clear();

    // clear the "PLAYER ONE" text
    vid_color_text(i2(9,14), 0x10, "          ");

    /* if a new round was started because Pacman has "won" (eaten all dots),
        redraw the playfield and reset the global dot counter
    */
    if (state.game.num_dots_eaten == NUM_DOTS) {
        state.game.round++;
        state.game.num_dots_eaten = 0;
        game_init_playfield();
        state.game.global_dot_counter_active = false;
    }
    else {
        /* if the previous round was lost, use the global dot counter
           to detect when ghosts should leave the ghost house instead
           of the per-ghost dot counter
        */
        if (state.game.num_lives != NUM_LIVES) {
            state.game.global_dot_counter_active = true;
            state.game.global_dot_counter = 0;
        }
        state.game.num_lives--;
    }
    assert(state.game.num_lives >= 0);

    state.game.active_fruit = FRUIT_NONE;
    state.game.freeze = FREEZETYPE_READY;
    state.game.xorshift = 0x12345678;   // random-number-generator seed
    state.game.num_ghosts_eaten = 0;
    game_disable_timers();

    vid_color_text(i2(11, 20), 0x9, "READY!");

    // the force-house timer forces ghosts out of the house if Pacman isn't
    // eating dots for a while
    start(&state.game.force_leave_house);

    // Pacman starts running to the left
    state.game.pacman1 = (pacman_t) {
        .actor = {
            .dir = DIR_LEFT,
            .pos = { 14*8, 26*8+4 },
        },
    };

    state.game.pacman2 = (pacman_t){
        .actor = {
            .dir = DIR_RIGHT,
            .pos = { 14 * 8, 26 * 8 + 4 },
        },
    };


    state.gfx.sprite[SPRITE_PACMAN] = (sprite_t) { .enabled = true, .color = COLOR_PACMAN };

    // Blinky starts outside the ghost house, looking to the left, and in scatter mode
    state.game.ghost[GHOSTTYPE_BLINKY] = (ghost_t) {
        .actor = {
            .dir = DIR_LEFT,
            .pos = ghost_starting_pos[GHOSTTYPE_BLINKY],
        },
        .type = GHOSTTYPE_BLINKY,
        .next_dir = DIR_LEFT,
        .state = GHOSTSTATE_SCATTER,
        .frightened = disabled_timer(),
        .eaten = disabled_timer(),
        .dot_counter = 0,
        .dot_limit = 0
    };
    state.gfx.sprite[SPRITE_BLINKY] = (sprite_t) { .enabled = true, .color = COLOR_BLINKY };

    // Pinky starts in the middle slot of the ghost house, moving down
    state.game.ghost[GHOSTTYPE_PINKY] = (ghost_t) {
        .actor = {
            .dir = DIR_DOWN,
            .pos = ghost_starting_pos[GHOSTTYPE_PINKY],
        },
        .type = GHOSTTYPE_PINKY,
        .next_dir = DIR_DOWN,
        .state = GHOSTSTATE_HOUSE,
        .frightened = disabled_timer(),
        .eaten = disabled_timer(),
        .dot_counter = 0,
        .dot_limit = 0
    };
    state.gfx.sprite[SPRITE_PINKY] = (sprite_t) { .enabled = true, .color = COLOR_PINKY };

    // Inky starts in the left slot of the ghost house moving up
    state.game.ghost[GHOSTTYPE_INKY] = (ghost_t) {
        .actor = {
            .dir = DIR_UP,
            .pos = ghost_starting_pos[GHOSTTYPE_INKY],
        },
        .type = GHOSTTYPE_INKY,
        .next_dir = DIR_UP,
        .state = GHOSTSTATE_HOUSE,
        .frightened = disabled_timer(),
        .eaten = disabled_timer(),
        .dot_counter = 0,
        // FIXME: needs to be adjusted by current round!
        .dot_limit = 30
    };
    state.gfx.sprite[SPRITE_INKY] = (sprite_t) { .enabled = true, .color = COLOR_INKY };

    // Clyde starts in the right slot of the ghost house, moving up
    state.game.ghost[GHOSTTYPE_CLYDE] = (ghost_t) {
        .actor = {
            .dir = DIR_UP,
            .pos = ghost_starting_pos[GHOSTTYPE_CLYDE],
        },
        .type = GHOSTTYPE_CLYDE,
        .next_dir = DIR_UP,
        .state = GHOSTSTATE_HOUSE,
        .frightened = disabled_timer(),
        .eaten = disabled_timer(),
        .dot_counter = 0,
        // FIXME: needs to be adjusted by current round!
        .dot_limit = 60,
    };
    state.gfx.sprite[SPRITE_CLYDE] = (sprite_t) { .enabled = true, .color = COLOR_CLYDE };
}

// update dynamic background tiles
static void game_update_tiles(void) {
    // print score and hiscore
    vid_color_score(i2(6,1), COLOR_DEFAULT, state.game.score);
    if (state.game.hiscore > 0) {
        vid_color_score(i2(16,1), COLOR_DEFAULT, state.game.hiscore);
    }

    // update the energizer pill colors (blinking/non-blinking)
    static const int2_t pill_pos[NUM_PILLS] = { { 1, 6 }, { 26, 6 }, { 1, 26 }, { 26, 26 } };
    for (int i = 0; i < NUM_PILLS; i++) {
        if (state.game.freeze) {
            vid_color(pill_pos[i], COLOR_DOT);
        }
        else {
            vid_color(pill_pos[i], (state.timing.tick & 0x8) ? 0x10:0);
        }
    }

    // clear the fruit-eaten score after Pacman has eaten a bonus fruit
    if (after_once(state.game.fruit_eaten, 2*60)) {
        vid_fruit_score(FRUIT_NONE);
    }

    // remaining lives at bottom left screen
    for (int i = 0; i < NUM_LIVES; i++) {
        uint8_t color = (i < state.game.num_lives) ? COLOR_PACMAN : 0;
        vid_draw_tile_quad(i2(2+2*i,34), color, TILE_LIFE);
    }

    // bonus fruit list in bottom-right corner
    {
        int16_t x = 24;
        for (int i = ((int)state.game.round - NUM_STATUS_FRUITS + 1); i <= (int)state.game.round; i++) {
            if (i >= 0) {
                fruit_t fruit = levelspec(i).bonus_fruit;
                uint8_t tile_code = fruit_tiles_colors[fruit][0];
                uint8_t color_code = fruit_tiles_colors[fruit][2];
                vid_draw_tile_quad(i2(x,34), color_code, tile_code);
                x -= 2;
            }
        }
    }

    // if game round was won, render the entire playfield as blinking blue/white
    if (after(state.game.round_won, 1*60)) {
        if (since(state.game.round_won) & 0x10) {
            vid_color_playfield(COLOR_DOT);
        }
        else {
            vid_color_playfield(COLOR_WHITE_BORDER);
        }
    }
}

// this function takes care of updating all sprite images during gameplay
static void game_update_sprites(void) {
    // update Pacman sprite
    {
        sprite_t* spr1 = spr_pacman();
        sprite_t* spr2 = spr_pacman();

        if (spr1->enabled) {
            const actor_t* actor1 = &state.game.pacman1.actor;
            const actor_t* actor2 = &state.game.pacman2.actor;

            /*


            if (state.game.player2) {
                spr1->pos = actor_to_sprite_pos(actor1->pos);
            }
            else {
                spr2->pos = actor_to_sprite_pos(actor2->pos);
            }

            if (state.game.freeze & FREEZETYPE_EAT_GHOST) {
                // hide Pacman shortly after he's eaten a ghost (via an invisible Sprite tile)

                spr1->tile = SPRITETILE_INVISIBLE;
                spr2->tile = SPRITETILE_INVISIBLE;

            }
            else if (state.game.freeze & (FREEZETYPE_PRELUDE|FREEZETYPE_READY)) {
                // special case game frozen at start of round, show Pacman with 'closed mouth'
                if (state.game.player2) {
                    spr1->tile = SPRITETILE_PACMAN_CLOSED_MOUTH;
                }
                else {
                    spr2->tile = SPRITETILE_PACMAN_CLOSED_MOUTH;
                }


                //spr1->tile = SPRITETILE_PACMAN_CLOSED_MOUTH;
                //spr2->tile = SPRITETILE_PACMAN_CLOSED_MOUTH;

            }
            else if (state.game.freeze & FREEZETYPE_DEAD) {
                // play the Pacman-death-animation after a short pause
                if (after(state.game.pacman_eaten, PACMAN_EATEN_TICKS)) {
                    spr_anim_pacman_death(since(state.game.pacman_eaten) - PACMAN_EATEN_TICKS);
                }
            }
            else {
                // regular Pacman animation
                if (state.game.player2) {
                    spr_anim_pacman(actor2->dir, actor2->anim_tick);

                }
                else {
                    spr_anim_pacman(actor1->dir, actor1->anim_tick);

                }
                //spr_anim_pacman(actor1->dir, actor1->anim_tick);
                //spr_anim_pacman(actor2->dir, actor2->anim_tick);

            }

*/



            if (state.game.player2) {
                spr1->pos = actor_to_sprite_pos(actor1->pos);
                if (state.game.freeze & FREEZETYPE_EAT_GHOST) {
                    // hide Pacman shortly after he's eaten a ghost (via an invisible Sprite tile)

                    spr1->tile = SPRITETILE_INVISIBLE;
                }
                else if (state.game.freeze & (FREEZETYPE_PRELUDE | FREEZETYPE_READY)) {
                    spr1->tile = SPRITETILE_PACMAN_CLOSED_MOUTH;
                }
                else if (state.game.freeze & FREEZETYPE_DEAD) {
                    // play the Pacman-death-animation after a short pause
                    if (after(state.game.pacman_eaten, PACMAN_EATEN_TICKS)) {
                        spr_anim_pacman_death(since(state.game.pacman_eaten) - PACMAN_EATEN_TICKS);
                    }
                }
                else {
                    spr_anim_pacman(actor1->dir, actor1->anim_tick);
                }
            }
            else {
                spr2->pos = actor_to_sprite_pos(actor2->pos);
                if (state.game.freeze & FREEZETYPE_EAT_GHOST) {
                    // hide Pacman shortly after he's eaten a ghost (via an invisible Sprite tile)

                    spr2->tile = SPRITETILE_INVISIBLE;
                }
                else if (state.game.freeze & (FREEZETYPE_PRELUDE | FREEZETYPE_READY)) {
                    spr2->tile = SPRITETILE_PACMAN_CLOSED_MOUTH;
                }
                else if (state.game.freeze & FREEZETYPE_DEAD) {
                    // play the Pacman-death-animation after a short pause
                    if (after(state.game.pacman_eaten, PACMAN_EATEN_TICKS)) {
                        spr_anim_pacman_death(since(state.game.pacman_eaten) - PACMAN_EATEN_TICKS);
                    }
                }
                else {
                    spr_anim_pacman(actor2->dir, actor2->anim_tick);
                }
            }
                
















        }
    }

    // update ghost sprites
    for (int i = 0; i < NUM_GHOSTS; i++) {
        sprite_t* sprite = spr_ghost(i);
        if (sprite->enabled) {
            const ghost_t* ghost = &state.game.ghost[i];
            sprite->pos = actor_to_sprite_pos(ghost->actor.pos);
            // if Pacman has just died, hide ghosts
            if (state.game.freeze & FREEZETYPE_DEAD) {
                if (after(state.game.pacman_eaten, PACMAN_EATEN_TICKS)) {
                    sprite->tile = SPRITETILE_INVISIBLE;
                }
            }
            // if Pacman has won the round, hide ghosts
            else if (state.game.freeze & FREEZETYPE_WON) {
                sprite->tile = SPRITETILE_INVISIBLE;
            }
            else switch (ghost->state) {
                case GHOSTSTATE_EYES:
                    if (before(ghost->eaten, GHOST_EATEN_FREEZE_TICKS)) {
                        // if the ghost was *just* eaten by Pacman, the ghost's sprite
                        // is replaced with a score number for a short time
                        // (200 for the first ghost, followed by 400, 800 and 1600)
                        sprite->tile = SPRITETILE_SCORE_200 + state.game.num_ghosts_eaten - 1;
                        sprite->color = COLOR_GHOST_SCORE;
                    }
                    else {
                        // afterwards, the ghost's eyes are shown, heading back to the ghost house
                        spr_anim_ghost_eyes(i, ghost->next_dir);
                    }
                    break;
                case GHOSTSTATE_ENTERHOUSE:
                    // ...still show the ghost eyes while entering the ghost house
                    spr_anim_ghost_eyes(i, ghost->actor.dir);
                    break;
                case GHOSTSTATE_FRIGHTENED:
                    // when inside the ghost house, show the normal ghost images
                    // (FIXME: ghost's inside the ghost house also show the
                    // frightened appearance when Pacman has eaten an energizer pill)
                    spr_anim_ghost_frightened(i, since(ghost->frightened));
                    break;
                default:
                    // show the regular ghost sprite image, the ghost's
                    // 'next_dir' is used to visualize the direction the ghost
                    // is heading to, this has the effect that ghosts already look
                    // into the direction they will move into one tile ahead
                    spr_anim_ghost(i, ghost->next_dir, ghost->actor.anim_tick);
                    break;
            }
        }
    }

    // hide or display the currently active bonus fruit
    if (state.game.active_fruit == FRUIT_NONE) {
        spr_fruit()->enabled = false;
    }
    else {
        sprite_t* spr = spr_fruit();
        spr->enabled = true;
        spr->pos = i2(13 * TILE_WIDTH, 19 * TILE_HEIGHT + TILE_HEIGHT/2);
        spr->tile = fruit_tiles_colors[state.game.active_fruit][1];
        spr->color = fruit_tiles_colors[state.game.active_fruit][2];
    }
}

// return true if Pacman should move in this tick, when eating dots, Pacman
// is slightly slower than ghosts, otherwise slightly faster
static bool game_pacman_should_move(void) {
    if (now(state.game.dot_eaten)) {
        // eating a dot causes Pacman to stop for 1 tick
        return false;
    }
    else if (since(state.game.pill_eaten) < 3) {
        // eating an energizer pill causes Pacman to stop for 3 ticks
        return false;
    }
    else {
        return 0 != (state.timing.tick % 8);
    }
}

// return number of pixels a ghost should move this tick, this can't be a simple
// move/don't move boolean return value, because ghosts in eye state move faster
// than one pixel per tick
static int game_ghost_speed(const ghost_t* ghost) {
    assert(ghost);
    switch (ghost->state) {
        case GHOSTSTATE_HOUSE:
        case GHOSTSTATE_LEAVEHOUSE:
            // inside house at half speed (estimated)
            return state.timing.tick & 1;
        case GHOSTSTATE_FRIGHTENED:
            // move at 50% speed when frightened
            return state.timing.tick & 1;
        case GHOSTSTATE_EYES:
        case GHOSTSTATE_ENTERHOUSE:
            // estimated 1.5x when in eye state, Pacman Dossier is silent on this
            return (state.timing.tick & 1) ? 1 : 2;
        default:
            if (is_tunnel(pixel_to_tile_pos(ghost->actor.pos))) {
                // move drastically slower when inside tunnel
                return ((state.timing.tick * 2) % 4) ? 1 : 0;
            }
            else {
                // otherwise move just a bit slower than Pacman
                return (state.timing.tick % 7) ? 1 : 0;
            }
    }
}

// return the current global scatter or chase phase
static ghoststate_t game_scatter_chase_phase(void) {
    uint32_t t = since(state.game.round_started);
    if (t < 7*60)       return GHOSTSTATE_SCATTER;
    else if (t < 27*60) return GHOSTSTATE_CHASE;
    else if (t < 34*60) return GHOSTSTATE_SCATTER;
    else if (t < 54*60) return GHOSTSTATE_CHASE;
    else if (t < 59*60) return GHOSTSTATE_SCATTER;
    else if (t < 79*60) return GHOSTSTATE_CHASE;
    else if (t < 84*60) return GHOSTSTATE_SCATTER;
    else return GHOSTSTATE_CHASE;
}

// this function takes care of switching ghosts into a new state, this is one
// of two important functions of the ghost AI (the other being the target selection
// function below)
static void game_update_ghost_state(ghost_t* ghost) {
    assert(ghost);
    ghoststate_t new_state = ghost->state;
    switch (ghost->state) {
        case GHOSTSTATE_EYES:
            // When in eye state (heading back to the ghost house), check if the
            // target position in front of the ghost house has been reached, then
            // switch into ENTERHOUSE state. Since ghosts in eye state move faster
            // than one pixel per tick, do a fuzzy comparison with the target pos
            if (nearequal_i2(ghost->actor.pos, i2(ANTEPORTAS_X, ANTEPORTAS_Y), 1)) {
                new_state = GHOSTSTATE_ENTERHOUSE;
            }
            break;
        case GHOSTSTATE_ENTERHOUSE:
            // Ghosts that enter the ghost house during the gameplay loop immediately
            // leave the house again after reaching their target position inside the house.
            if (nearequal_i2(ghost->actor.pos, ghost_house_target_pos[ghost->type], 1)) {
                new_state = GHOSTSTATE_LEAVEHOUSE;
            }
            break;
        case GHOSTSTATE_HOUSE:
            // Ghosts only remain in the "house state" after a new game round
            // has been started. The conditions when ghosts leave the house
            // are a bit complicated, best to check the Pacman Dossier for the details.
            if (after_once(state.game.force_leave_house, 4*60)) {
                // if Pacman hasn't eaten dots for 4 seconds, the next ghost
                // is forced out of the house
                // FIXME: time is reduced to 3 seconds after round 5
                new_state = GHOSTSTATE_LEAVEHOUSE;
                start(&state.game.force_leave_house);
            }
            else if (state.game.global_dot_counter_active) {
                // if Pacman has lost a life this round, the global dot counter is used
                if ((ghost->type == GHOSTTYPE_PINKY) && (state.game.global_dot_counter == 7)) {
                    new_state = GHOSTSTATE_LEAVEHOUSE;
                }
                else if ((ghost->type == GHOSTTYPE_INKY) && (state.game.global_dot_counter == 17)) {
                    new_state = GHOSTSTATE_LEAVEHOUSE;
                }
                else if ((ghost->type == GHOSTTYPE_CLYDE) && (state.game.global_dot_counter == 32)) {
                    new_state = GHOSTSTATE_LEAVEHOUSE;
                    // NOTE that global dot counter is deactivated if (and only if) Clyde
                    // is in the house and the dot counter reaches 32
                    state.game.global_dot_counter_active = false;
                }
            }
            else if (ghost->dot_counter == ghost->dot_limit) {
                // in the normal case, check the ghost's personal dot counter
                new_state = GHOSTSTATE_LEAVEHOUSE;
            }
            break;
        case GHOSTSTATE_LEAVEHOUSE:
            // ghosts immediately switch to scatter mode after leaving the ghost house
            if (ghost->actor.pos.y == ANTEPORTAS_Y) {
                new_state = GHOSTSTATE_SCATTER;
            }
            break;
        default:
            // switch between frightened, scatter and chase mode
            if (before(ghost->frightened, levelspec(state.game.round).fright_ticks)) {
                new_state = GHOSTSTATE_FRIGHTENED;
            }
            else {
                new_state = game_scatter_chase_phase();
            }
    }
    // handle state transitions
    if (new_state != ghost->state) {
        switch (ghost->state) {
            case GHOSTSTATE_LEAVEHOUSE:
                // after leaving the ghost house, head to the left
                ghost->next_dir = ghost->actor.dir = DIR_LEFT;
                break;
            case GHOSTSTATE_ENTERHOUSE:
                // a ghost that was eaten is immune to frighten until Pacman eats enother pill
                disable(&ghost->frightened);
                break;
            case GHOSTSTATE_FRIGHTENED:
                // don't reverse direction when leaving frightened state
                break;
            case GHOSTSTATE_SCATTER:
            case GHOSTSTATE_CHASE:
                // any transition from scatter and chase mode causes a reversal of direction
                ghost->next_dir = reverse_dir(ghost->actor.dir);
                break;
            default:
                break;
        }
        ghost->state = new_state;
    }
}

// update the ghost's target position, this is the other important function
// of the ghost's AI
static void game_update_ghost_target(ghost_t* ghost) {
    assert(ghost);
    int2_t pos = ghost->target_pos;
    switch (ghost->state) {
        case GHOSTSTATE_SCATTER:
            // when in scatter mode, each ghost heads to its own scatter
            // target position in the playfield corners
            assert((ghost->type >= 0) && (ghost->type < NUM_GHOSTS));
            pos = ghost_scatter_targets[ghost->type];
            break;
        case GHOSTSTATE_CHASE:
            // when in chase mode, each ghost has its own particular
            // chase behaviour (see the Pacman Dossier for details)
            {
                const actor_t* pm1 = &state.game.pacman1.actor;
                const int2_t pm1_pos = pixel_to_tile_pos(pm1->pos);
                const int2_t pm1_dir = dir_to_vec(pm1->dir);

                const actor_t* pm2 = &state.game.pacman2.actor;
                const int2_t pm2_pos = pixel_to_tile_pos(pm2->pos);
                const int2_t pm2_dir = dir_to_vec(pm2->dir);

                switch (ghost->type) {
                    case GHOSTTYPE_BLINKY:
                        // Blinky directly chases Pacman
                        pos = pm1_pos;
                        pos = pm2_pos;

                        break;
                    case GHOSTTYPE_PINKY:
                        // Pinky target is 4 tiles ahead of Pacman
                        // FIXME: does not reproduce 'diagonal overflow'
                        pos = add_i2(pm1_pos, mul_i2(pm1_dir, 4));
                        pos = add_i2(pm2_pos, mul_i2(pm2_dir, 4));

                        break;
                    case GHOSTTYPE_INKY:
                        // Inky targets an extrapolated pos along a line two tiles
                        // ahead of Pacman through Blinky
                        {
                            const int2_t blinky_pos = pixel_to_tile_pos(state.game.ghost[GHOSTTYPE_BLINKY].actor.pos);
                            const int2_t p = add_i2(pm1_pos, mul_i2(pm1_dir, 2));

                            const int2_t d = sub_i2(p, blinky_pos);
                            pos = add_i2(blinky_pos, mul_i2(d, 2));
                        }
                        break;
                    case GHOSTTYPE_CLYDE:
                        // if Clyde is far away from Pacman, he chases Pacman,
                        // but if close he moves towards the scatter target
                        if (squared_distance_i2(pixel_to_tile_pos(ghost->actor.pos), pm1_pos) > 64) {
                            pos = pm1_pos;
                        }
                        else if (squared_distance_i2(pixel_to_tile_pos(ghost->actor.pos), pm2_pos) > 64) {
                            pos = pm2_pos;
                        }
                        else {
                            pos = ghost_scatter_targets[GHOSTTYPE_CLYDE];
                        }
                        break;
                    default:
                        break;
                }
            }
            break;
        case GHOSTSTATE_FRIGHTENED:
            // in frightened state just select a random target position
            // this has the effect that ghosts in frightened state
            // move in a random direction at each intersection
            pos = i2(xorshift32() % DISPLAY_TILES_X, xorshift32() % DISPLAY_TILES_Y);
            break;
        case GHOSTSTATE_EYES:
            // move towards the ghost house door
            pos = i2(13, 14);
            break;
        default:
            break;
    }
    ghost->target_pos = pos;
}

// compute the next ghost direction, return true if resulting movement
// should always happen regardless of current ghost position or blocking
// tiles (this special case is used for movement inside the ghost house)
static bool game_update_ghost_dir(ghost_t* ghost) {
    assert(ghost);
    // inside ghost-house, just move up and down
    if (ghost->state == GHOSTSTATE_HOUSE) {
        if (ghost->actor.pos.y <= 17*TILE_HEIGHT) {
            ghost->next_dir = DIR_DOWN;
        }
        else if (ghost->actor.pos.y >= 18*TILE_HEIGHT) {
            ghost->next_dir = DIR_UP;
        }
        ghost->actor.dir = ghost->next_dir;
        // force movement
        return true;
    }
    // navigate the ghost out of the ghost house
    else if (ghost->state == GHOSTSTATE_LEAVEHOUSE) {
        const int2_t pos = ghost->actor.pos;
        if (pos.x == ANTEPORTAS_X) {
            if (pos.y > ANTEPORTAS_Y) {
                ghost->next_dir = DIR_UP;
            }
        }
        else {
            const int16_t mid_y = 17*TILE_HEIGHT + TILE_HEIGHT/2;
            if (pos.y > mid_y) {
                ghost->next_dir = DIR_UP;
            }
            else if (pos.y < mid_y) {
                ghost->next_dir = DIR_DOWN;
            }
            else {
                ghost->next_dir = (pos.x > ANTEPORTAS_X) ? DIR_LEFT:DIR_RIGHT;
            }
        }
        ghost->actor.dir = ghost->next_dir;
        return true;
    }
    // navigate towards the ghost house target pos
    else if (ghost->state == GHOSTSTATE_ENTERHOUSE) {
        const int2_t pos = ghost->actor.pos;
        const int2_t tile_pos = pixel_to_tile_pos(pos);
        const int2_t tgt_pos = ghost_house_target_pos[ghost->type];
        if (tile_pos.y == 14) {
            if (pos.x != ANTEPORTAS_X) {
                ghost->next_dir = (pos.x < ANTEPORTAS_X) ? DIR_RIGHT:DIR_LEFT;
            }
            else {
                ghost->next_dir = DIR_DOWN;
            }
        }
        else if (pos.y == tgt_pos.y) {
            ghost->next_dir = (pos.x < tgt_pos.x) ? DIR_RIGHT:DIR_LEFT;
        }
        ghost->actor.dir = ghost->next_dir;
        return true;
    }
    // scatter/chase/frightened: just head towards the current target point
    else {
        // only compute new direction when currently at midpoint of tile
        const int2_t dist_to_mid = dist_to_tile_mid(ghost->actor.pos);
        if ((dist_to_mid.x == 0) && (dist_to_mid.y == 0)) {
            // new direction is the previously computed next-direction
            ghost->actor.dir = ghost->next_dir;

            // compute new next-direction
            const int2_t dir_vec = dir_to_vec(ghost->actor.dir);
            const int2_t lookahead_pos = add_i2(pixel_to_tile_pos(ghost->actor.pos), dir_vec);

            // try each direction and take the one that moves closest to the target
            const dir_t dirs[NUM_DIRS] = { DIR_UP, DIR_LEFT, DIR_DOWN, DIR_RIGHT };
            int min_dist = 100000;
            int dist = 0;
            for (int i = 0; i < NUM_DIRS; i++) {
                const dir_t dir = dirs[i];
                // if ghost is in one of the two 'red zones', forbid upward movement
                // (see Pacman Dossier "Areas To Exploit")
                if (is_redzone(lookahead_pos) && (dir == DIR_UP) && (ghost->state != GHOSTSTATE_EYES)) {
                    continue;
                }
                const dir_t revdir = reverse_dir(dir);
                const int2_t test_pos = clamped_tile_pos(add_i2(lookahead_pos, dir_to_vec(dir)));
                if ((revdir != ghost->actor.dir) && !is_blocking_tile(test_pos)) {
                    if ((dist = squared_distance_i2(test_pos, ghost->target_pos)) < min_dist) {
                        min_dist = dist;
                        ghost->next_dir = dir;
                    }
                }
            }
        }
        return false;
    }
}

/* Update the dot counters used to decide whether ghosts must leave the house.

    This is called each time Pacman eats a dot.

    Each ghost has a dot limit which is reset at the start of a round. Each time
    Pacman eats a dot, the highest priority ghost in the ghost house counts
    down its dot counter.

    When the ghost's dot counter reaches zero the ghost leaves the house
    and the next highest-priority dot counter starts counting.

    If a life is lost, the personal dot counters are deactivated and instead
    a global dot counter is used.

    If pacman doesn't eat dots for a while, the next ghost is forced out of the
    house using a timer.
*/
static void game_update_ghosthouse_dot_counters(void) {
    // if the new round was started because Pacman lost a life, use the global
    // dot counter (this mode will be deactivated again after all ghosts left the
    // house)
    if (state.game.global_dot_counter_active) {
        state.game.global_dot_counter++;
    }
    else {
        // otherwise each ghost has his own personal dot counter to decide
        // when to leave the ghost house
        for (int i = 0; i < NUM_GHOSTS; i++) {
            if (state.game.ghost[i].dot_counter < state.game.ghost[i].dot_limit) {
                state.game.ghost[i].dot_counter++;
                break;
            }
        }
    }
}

// called when a dot or pill has been eaten, checks if a round has been won
// (all dots and pills eaten), whether to show the bonus fruit, and finally
// plays the dot-eaten sound effect
static void game_update_dots_eaten(void) {
    state.game.num_dots_eaten++;
    if (state.game.num_dots_eaten == NUM_DOTS) {
        // all dots eaten, round won
        start(&state.game.round_won);
        snd_clear();
    }
    else if ((state.game.num_dots_eaten == 70) || (state.game.num_dots_eaten == 170)) {
        // at 70 and 170 dots, show the bonus fruit
        start(&state.game.fruit_active);
    }

    // play alternating crunch sound effect when a dot has been eaten
    if (state.game.num_dots_eaten & 1) {
        snd_start(2, &snd_eatdot1);
    }
    else {
        snd_start(2, &snd_eatdot2);
    }
}

// the central Pacman and ghost behaviour function, called once per game tick
static void game_update_actors(void) {
    // Pacman "AI"
    if (game_pacman_should_move() && state.game.player2) {
        // move Pacman with cornering allowed
        actor_t* actor1 = &state.game.pacman1.actor;
        const dir_t wanted_dir = input_dir(actor1->dir);
        const bool allow_cornering = true;
        // look ahead to check if the wanted direction is blocked
        if (can_move(actor1->pos, wanted_dir, allow_cornering)) {
            actor1->dir = wanted_dir;
        }
        // move into the selected direction
        if (can_move(actor1->pos, actor1->dir, allow_cornering)) {
            actor1->pos = move(actor1->pos, actor1->dir, allow_cornering);
            actor1->anim_tick++;
        }
        // eat dot or energizer pill?
        const int2_t tile_pos = pixel_to_tile_pos(actor1->pos);
        if (is_dot(tile_pos)) {
            vid_tile(tile_pos, TILE_SPACE);
            state.game.score += 1;
            start(&state.game.dot_eaten);
            start(&state.game.force_leave_house);
            game_update_dots_eaten();
            game_update_ghosthouse_dot_counters();
        }
        if (is_pill(tile_pos)) {
            vid_tile(tile_pos, TILE_SPACE);
            state.game.score += 5;
            game_update_dots_eaten();
            start(&state.game.pill_eaten);
            state.game.num_ghosts_eaten = 0;
            for (int i = 0; i < NUM_GHOSTS; i++) {
                start(&state.game.ghost[i].frightened);
            }
            snd_start(1, &snd_frightened);
        }
        // check if Pacman eats the bonus fruit
        if (state.game.active_fruit != FRUIT_NONE) {
            const int2_t test_pos = pixel_to_tile_pos(add_i2(actor1->pos, i2(TILE_WIDTH/2, 0)));
            if (equal_i2(test_pos, i2(14, 20))) {
                start(&state.game.fruit_eaten);
                uint32_t score = levelspec(state.game.round).bonus_score;
                state.game.score += score;
                vid_fruit_score(state.game.active_fruit);
                state.game.active_fruit = FRUIT_NONE;
                snd_start(2, &snd_eatfruit);
                //Added by Tommy Pham
                start(&state.game.pill_eaten);
                state.game.num_ghosts_eaten = 0;
                for (int i = 0; i < NUM_GHOSTS; i++) {
                    start(&state.game.ghost[i].frightened);

                }
                snd_start(1, &snd_frightened);
                
            }
        }
        // check if Pacman collides with any ghost
        for (int i = 0; i < NUM_GHOSTS; i++) {
            ghost_t* ghost = &state.game.ghost[i];
            const int2_t ghost_tile_pos = pixel_to_tile_pos(ghost->actor.pos);
            if (equal_i2(tile_pos, ghost_tile_pos)) {
                if (ghost->state == GHOSTSTATE_FRIGHTENED) {
                    // Pacman eats a frightened ghost
                    ghost->state = GHOSTSTATE_EYES;
                    start(&ghost->eaten);
                    start(&state.game.ghost_eaten);
                    state.game.num_ghosts_eaten++;
                    // increase score by 20, 40, 80, 160
                    state.game.score += 10 * (1<<state.game.num_ghosts_eaten);
                    state.game.freeze |= FREEZETYPE_EAT_GHOST;
                    snd_start(2, &snd_eatghost);
                }
                else if ((ghost->state == GHOSTSTATE_CHASE) || (ghost->state == GHOSTSTATE_SCATTER)) {
                    // otherwise, ghost eats Pacman, Pacman loses a life
                    #if !DBG_GODMODE
                    snd_clear();
                    start(&state.game.pacman_eaten);
                    state.game.freeze |= FREEZETYPE_DEAD;
                    // if Pacman has any lives left start a new round, otherwise start the game-over sequence
                    if (state.game.num_lives > 0) {
                        start_after(&state.game.ready_started, PACMAN_EATEN_TICKS+PACMAN_DEATH_TICKS);
                    }
                    else {
                        start_after(&state.game.game_over, PACMAN_EATEN_TICKS+PACMAN_DEATH_TICKS);
                    }
                    #endif
                }
            }
        }
    }


    if (game_pacman_should_move() && !state.game.player2) {
        // move Pacman with cornering allowed
        actor_t* actor2 = &state.game.pacman2.actor;
        const dir_t wanted_dir = input_dir(actor2->dir);
        const bool allow_cornering = true;
        // look ahead to check if the wanted direction is blocked
        if (can_move(actor2->pos, wanted_dir, allow_cornering)) {
            actor2->dir = wanted_dir;
        }
        // move into the selected direction
        if (can_move(actor2->pos, actor2->dir, allow_cornering)) {
            actor2->pos = move(actor2->pos, actor2->dir, allow_cornering);
            actor2->anim_tick++;
        }
        // eat dot or energizer pill?
        const int2_t tile_pos = pixel_to_tile_pos(actor2->pos);
        if (is_dot(tile_pos)) {
            vid_tile(tile_pos, TILE_SPACE);
            state.game.score += 1;
            start(&state.game.dot_eaten);
            start(&state.game.force_leave_house);
            game_update_dots_eaten();
            game_update_ghosthouse_dot_counters();
        }
        if (is_pill(tile_pos)) {
            vid_tile(tile_pos, TILE_SPACE);
            state.game.score += 5;
            game_update_dots_eaten();
            start(&state.game.pill_eaten);
            state.game.num_ghosts_eaten = 0;
            for (int i = 0; i < NUM_GHOSTS; i++) {
                start(&state.game.ghost[i].frightened);
            }
            snd_start(1, &snd_frightened);
        }
        // check if Pacman eats the bonus fruit
        if (state.game.active_fruit != FRUIT_NONE) {
            const int2_t test_pos = pixel_to_tile_pos(add_i2(actor2->pos, i2(TILE_WIDTH / 2, 0)));
            if (equal_i2(test_pos, i2(14, 20))) {
                start(&state.game.fruit_eaten);
                uint32_t score = levelspec(state.game.round).bonus_score;
                state.game.score += score;
                vid_fruit_score(state.game.active_fruit);
                state.game.active_fruit = FRUIT_NONE;
                snd_start(2, &snd_eatfruit);
            }
        }
        // check if Pacman collides with any ghost
        for (int i = 0; i < NUM_GHOSTS; i++) {
            ghost_t* ghost = &state.game.ghost[i];
            const int2_t ghost_tile_pos = pixel_to_tile_pos(ghost->actor.pos);
            if (equal_i2(tile_pos, ghost_tile_pos)) {
                if (ghost->state == GHOSTSTATE_FRIGHTENED) {
                    // Pacman eats a frightened ghost
                    ghost->state = GHOSTSTATE_EYES;
                    start(&ghost->eaten);
                    start(&state.game.ghost_eaten);
                    state.game.num_ghosts_eaten++;
                    // increase score by 20, 40, 80, 160
                    state.game.score += 10 * (1 << state.game.num_ghosts_eaten);
                    state.game.freeze |= FREEZETYPE_EAT_GHOST;
                    snd_start(2, &snd_eatghost);
                }
                else if ((ghost->state == GHOSTSTATE_CHASE) || (ghost->state == GHOSTSTATE_SCATTER)) {
                    // otherwise, ghost eats Pacman, Pacman loses a life
#if !DBG_GODMODE
                    snd_clear();
                    start(&state.game.pacman_eaten);
                    state.game.freeze |= FREEZETYPE_DEAD;
                    // if Pacman has any lives left start a new round, otherwise start the game-over sequence
                    if (state.game.num_lives > 0) {
                        start_after(&state.game.ready_started, PACMAN_EATEN_TICKS + PACMAN_DEATH_TICKS);
                    }
                    else {
                        start_after(&state.game.game_over, PACMAN_EATEN_TICKS + PACMAN_DEATH_TICKS);
                    }
#endif
                }
            }
        }
    }

    // Ghost "AIs"
    for (int ghost_index = 0; ghost_index < NUM_GHOSTS; ghost_index++) {
        ghost_t* ghost = &state.game.ghost[ghost_index];
        // handle ghost-state transitions
        game_update_ghost_state(ghost);
        // update the ghost's target position
        game_update_ghost_target(ghost);
        // finally, move the ghost towards the current target position
        const int num_move_ticks = game_ghost_speed(ghost);
        for (int i = 0; i < num_move_ticks; i++) {
            bool force_move = game_update_ghost_dir(ghost);
            actor_t* actor = &ghost->actor;
            const bool allow_cornering = false;
            if (force_move || can_move(actor->pos, actor->dir, allow_cornering)) {
                actor->pos = move(actor->pos, actor->dir, allow_cornering);
                actor->anim_tick++;
            }
        }
    }
}

// the central game tick function, called at 60 Hz
static void game_tick(void) {
    // debug: skip prelude
    #if DBG_SKIP_PRELUDE
        const int prelude_ticks_per_sec = 1;
    #else
        const int prelude_ticks_per_sec = 60;
    #endif

    // initialize game state once
    if (now(state.game.started)) {
        start(&state.gfx.fadein);
        start_after(&state.game.ready_started, 2*prelude_ticks_per_sec);
        snd_start(0, &snd_prelude);
        game_init();
    }
    // initialize new round (each time Pacman looses a life), make actors visible, remove "PLAYER ONE", start a new life
    if (now(state.game.ready_started)) {
        game_round_init();
        // after 2 seconds start the interactive game loop
        start_after(&state.game.round_started, 2*60+10);
    }
    if (now(state.game.round_started)) {
        state.game.freeze &= ~FREEZETYPE_READY;
        // clear the 'READY!' message
        vid_color_text(i2(11,20), 0x10, "      ");
        snd_start(1, &snd_weeooh);
    }

    // activate/deactivate bonus fruit
    if (now(state.game.fruit_active)) {
        state.game.active_fruit = levelspec(state.game.round).bonus_fruit;
    }
    else if (after_once(state.game.fruit_active, FRUITACTIVE_TICKS)) {
        state.game.active_fruit = FRUIT_NONE;
    }

    // stop frightened sound and start weeooh sound
    if (after_once(state.game.pill_eaten, levelspec(state.game.round).fright_ticks)) {
        snd_start(1, &snd_weeooh);
    }

    // if game is frozen because Pacman ate a ghost, unfreeze after a while
    if (state.game.freeze & FREEZETYPE_EAT_GHOST) {
        if (after_once(state.game.ghost_eaten, GHOST_EATEN_FREEZE_TICKS)) {
            state.game.freeze &= ~FREEZETYPE_EAT_GHOST;
        }
    }

    // play pacman-death sound
    if (after_once(state.game.pacman_eaten, PACMAN_EATEN_TICKS)) {
        snd_start(2, &snd_dead);
    }

    // the actually important part: update Pacman and ghosts, update dynamic
    // background tiles, and update the sprite images
    if (!state.game.freeze) {
        game_update_actors();
    }
    game_update_tiles();
    game_update_sprites();

    // update hiscore
    if (state.game.score > state.game.hiscore) {
        state.game.hiscore = state.game.score;
    }

    // check for end-round condition
    if (now(state.game.round_won)) {
        state.game.freeze |= FREEZETYPE_WON;
        start_after(&state.game.ready_started, ROUNDWON_TICKS);
    }
    if (now(state.game.game_over)) {
        // display game over string
        vid_color_text(i2(9,20), 0x01, "GAME  OVER");
        input_disable();
        start_after(&state.gfx.fadeout, GAMEOVER_TICKS);
        start_after(&state.intro.started, GAMEOVER_TICKS+FADE_TICKS);
    }

    #if DBG_ESCAPE
        if (state.input1.esc) {
            input_disable();
            start(&state.gfx.fadeout);
            start_after(&state.intro.started, FADE_TICKS);
        }
    #endif

    #if DBG_MARKERS
        // visualize current ghost targets
        for (int i = 0; i < NUM_GHOSTS; i++) {
            const ghost_t* ghost = &state.game.ghost[i];
            uint8_t tile = 'X';
            switch (ghost->state) {
                case GHOSTSTATE_NONE:       tile = 'N'; break;
                case GHOSTSTATE_CHASE:      tile = 'C'; break;
                case GHOSTSTATE_SCATTER:    tile = 'S'; break;
                case GHOSTSTATE_FRIGHTENED: tile = 'F'; break;
                case GHOSTSTATE_EYES:       tile = 'E'; break;
                case GHOSTSTATE_HOUSE:      tile = 'H'; break;
                case GHOSTSTATE_LEAVEHOUSE: tile = 'L'; break;
                case GHOSTSTATE_ENTERHOUSE: tile = 'E'; break;
            }
            dbg_marker(i, state.game.ghost[i].target_pos, tile, COLOR_BLINKY+2*i);
        }
    #endif
}

/*== INTRO GAMESTATE CODE ====================================================*/

static void intro_tick(void) {

    // on intro-state enter, enable input and draw any initial text
    if (now(state.intro.started)) {
        snd_clear();
        spr_clear();
        start(&state.gfx.fadein);
        input_enable();
        vid_clear(TILE_SPACE, COLOR_DEFAULT);
        vid_text(i2(3,0),  "1UP   HIGH SCORE   2UP");
        vid_color_score(i2(6,1), COLOR_DEFAULT, 0);
        if (state.game.hiscore > 0) {
            vid_color_score(i2(16,1), COLOR_DEFAULT, state.game.hiscore);
        }
        vid_text(i2(7,5),  "CHARACTER / NICKNAME");
        vid_text(i2(3,35), "CREDIT  0");
    }

    // draw the animated 'ghost image.. name.. nickname' lines
    uint32_t delay = 30;
    //const char* names[] = { "-SHADOW", "-SPEEDY", "-BASHFUL", "-POKEY" };
    const char* names[] = { "-TOMMY", "-AUTUMN", "-MIKE", "-UNIX" };
    const char* nicknames[] = { "BLINKY", "PINKY", "INKY", "CLYDE" };
    for (int i = 0; i < 4; i++) {
        const uint8_t color = 2*i + 1;
        const uint8_t y = 3*i + 6;
        // 2*3 ghost image created from tiles (no sprite!)
        delay += 30;
        if (after_once(state.intro.started, delay)) {
            vid_color_tile(i2(4,y+0), color, TILE_GHOST+0); vid_color_tile(i2(5,y+0), color, TILE_GHOST+1);
            vid_color_tile(i2(4,y+1), color, TILE_GHOST+2); vid_color_tile(i2(5,y+1), color, TILE_GHOST+3);
            vid_color_tile(i2(4,y+2), color, TILE_GHOST+4); vid_color_tile(i2(5,y+2), color, TILE_GHOST+5);
        }
        // after 1 second, the name of the ghost
        delay += 60;
        if (after_once(state.intro.started, delay)) {
            vid_color_text(i2(7,y+1), color, names[i]);
        }
        // after 0.5 seconds, the nickname of the ghost
        delay += 30;
        if (after_once(state.intro.started, delay)) {
            vid_color_text(i2(17,y+1), color, nicknames[i]);
        }
    }

    // . 10 PTS
    // O 50 PTS
    delay += 60;
    if (after_once(state.intro.started, delay)) {
        vid_color_tile(i2(10,24), COLOR_DOT, TILE_DOT);
        vid_text(i2(12,24), "10 \x5D\x5E\x5F");
        vid_color_tile(i2(10,26), COLOR_DOT, TILE_PILL);
        vid_text(i2(12,26), "50 \x5D\x5E\x5F");
    }

    // blinking "press any key" text
    delay += 60;
    if (after(state.intro.started, delay)) {
        if (since(state.intro.started) & 0x20) {
            vid_color_text(i2(3,31), 3, "                       ");
        }
        else {
            vid_color_text(i2(3,31), 3, "PRESS ANY KEY TO START!");
        }
    }

    // FIXME: animated chase sequence

    // if a key is pressed, advance to game state
    if (state.input1.anykey) {
        input_disable();
        start(&state.gfx.fadeout);
        start_after(&state.game.started, FADE_TICKS);
    }
}

/*== GFX SUBSYSTEM ===========================================================*/
////////////////////////IMAGES AND PIXELING/////////////////////////////////////////
/* create all sokol-gfx resources */
static void gfx_create_resources(void) {
    // pass action for clearing the background to black
    state.gfx.pass_action = (sg_pass_action) {
        .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0.0f, 0.0f, 0.0f, 1.0f } }
    };

    // create a dynamic vertex buffer for the tile and sprite quads
    state.gfx.offscreen.vbuf = sg_make_buffer(&(sg_buffer_desc){
        .type = SG_BUFFERTYPE_VERTEXBUFFER,
        .usage = SG_USAGE_STREAM,
        .size = sizeof(state.gfx.vertices),
    });

    // create a simple quad vertex buffer for rendering the offscreen render target to the display
    float quad_verts[]= { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    state.gfx.display.quad_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(quad_verts)
    });

    // shader sources for all platforms (FIXME: should we use precompiled shader blobs instead?)
    const char* offscreen_vs_src = 0;
    const char* offscreen_fs_src = 0;
    const char* display_vs_src = 0;
    const char* display_fs_src = 0;
    switch (sg_query_backend()) {
        case SG_BACKEND_METAL_MACOS:
            offscreen_vs_src =
                "#include <metal_stdlib>\n"
                "using namespace metal;\n"
                "struct vs_in {\n"
                "  float4 pos [[attribute(0)]];\n"
                "  float2 uv [[attribute(1)]];\n"
                "  float4 data [[attribute(2)]];\n"
                "};\n"
                "struct vs_out {\n"
                "  float4 pos [[position]];\n"
                "  float2 uv;\n"
                "  float4 data;\n"
                "};\n"
                "vertex vs_out _main(vs_in in [[stage_in]]) {\n"
                "  vs_out out;\n"
                "  out.pos = float4((in.pos.xy - 0.5) * float2(2.0, -2.0), 0.5, 1.0);\n"
                "  out.uv  = in.uv;"
                "  out.data = in.data;\n"
                "  return out;\n"
                "}\n";
            offscreen_fs_src =
                "#include <metal_stdlib>\n"
                "using namespace metal;\n"
                "struct ps_in {\n"
                "  float2 uv;\n"
                "  float4 data;\n"
                "};\n"
                "fragment float4 _main(ps_in in [[stage_in]],\n"
                "                      texture2d<float> tile_tex [[texture(0)]],\n"
                "                      texture2d<float> pal_tex [[texture(1)]],\n"
                "                      sampler tile_smp [[sampler(0)]],\n"
                "                      sampler pal_smp [[sampler(1)]])\n"
                "{\n"
                "  float color_code = in.data.x;\n" // (0..31) / 255
                "  float tile_color = tile_tex.sample(tile_smp, in.uv).x;\n" // (0..3) / 255
                "  float2 pal_uv = float2(color_code * 4 + tile_color, 0);\n"
                "  float4 color = pal_tex.sample(pal_smp, pal_uv) * float4(1, 1, 1, in.data.y);\n"
                "  return color;\n"
                "}\n";
            display_vs_src =
                "#include <metal_stdlib>\n"
                "using namespace metal;\n"
                "struct vs_in {\n"
                "  float4 pos [[attribute(0)]];\n"
                "};\n"
                "struct vs_out {\n"
                "  float4 pos [[position]];\n"
                "  float2 uv;\n"
                "};\n"
                "vertex vs_out _main(vs_in in[[stage_in]]) {\n"
                "  vs_out out;\n"
                "  out.pos = float4((in.pos.xy - 0.5) * float2(2.0, -2.0), 0.0, 1.0);\n"
                "  out.uv = in.pos.xy;\n"
                "  return out;\n"
                "}\n";
            display_fs_src =
                "#include <metal_stdlib>\n"
                "using namespace metal;\n"
                "struct ps_in {\n"
                "  float2 uv;\n"
                "};\n"
                "fragment float4 _main(ps_in in [[stage_in]],\n"
                "                      texture2d<float> tex [[texture(0)]],\n"
                "                      sampler smp [[sampler(0)]])\n"
                "{\n"
                "  return tex.sample(smp, in.uv);\n"
                "}\n";
            break;
        case SG_BACKEND_D3D11:
            offscreen_vs_src =
                "struct vs_in {\n"
                "  float4 pos: POSITION;\n"
                "  float2 uv: TEXCOORD0;\n"
                "  float4 data: TEXCOORD1;\n"
                "};\n"
                "struct vs_out {\n"
                "  float2 uv: UV;\n"
                "  float4 data: DATA;\n"
                "  float4 pos: SV_Position;\n"
                "};\n"
                "vs_out main(vs_in inp) {\n"
                "  vs_out outp;"
                "  outp.pos = float4(inp.pos.xy * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
                "  outp.uv  = inp.uv;"
                "  outp.data = inp.data;\n"
                "  return outp;\n"
                "}\n";
            offscreen_fs_src =
                "Texture2D<float4> tile_tex: register(t0);\n"
                "Texture2D<float4> pal_tex: register(t1);\n"
                "sampler tile_smp: register(s0);\n"
                "sampler pal_smp: register(s1);\n"
                "float4 main(float2 uv: UV, float4 data: DATA): SV_Target0 {\n"
                "  float color_code = data.x;\n"
                "  float tile_color = tile_tex.Sample(tile_smp, uv).x;\n"
                "  float2 pal_uv = float2(color_code * 4 + tile_color, 0);\n"
                "  float4 color = pal_tex.Sample(pal_smp, pal_uv) * float4(1, 1, 1, data.y);\n"
                "  return color;\n"
                "}\n";
            display_vs_src =
                "struct vs_out {\n"
                "  float2 uv: UV;\n"
                "  float4 pos: SV_Position;\n"
                "};\n"
                "vs_out main(float4 pos: POSITION) {\n"
                "  vs_out outp;\n"
                "  outp.pos = float4((pos.xy - 0.5) * float2(2.0, -2.0), 0.0, 1.0);\n"
                "  outp.uv = pos.xy;\n"
                "  return outp;\n"
                "}\n";
            display_fs_src =
                "Texture2D<float4> tex: register(t0);\n"
                "sampler smp: register(s0);\n"
                "float4 main(float2 uv: UV): SV_Target0 {\n"
                "  return tex.Sample(smp, uv);\n"
                "}\n";
            break;
        case SG_BACKEND_GLCORE33:
            offscreen_vs_src =
                "#version 330\n"
                "layout(location=0) in vec4 pos;\n"
                "layout(location=1) in vec2 uv_in;\n"
                "layout(location=2) in vec4 data_in;\n"
                "out vec2 uv;\n"
                "out vec4 data;\n"
                "void main() {\n"
                "  gl_Position = vec4((pos.xy - 0.5) * vec2(2.0, -2.0), 0.5, 1.0);\n"
                "  uv  = uv_in;"
                "  data = data_in;\n"
                "}\n";
            offscreen_fs_src =
                "#version 330\n"
                "uniform sampler2D tile_tex;\n"
                "uniform sampler2D pal_tex;\n"
                "in vec2 uv;\n"
                "in vec4 data;\n"
                "out vec4 frag_color;\n"
                "void main() {\n"
                "  float color_code = data.x;\n"
                "  float tile_color = texture(tile_tex, uv).x;\n"
                "  vec2 pal_uv = vec2(color_code * 4 + tile_color, 0);\n"
                "  frag_color = texture(pal_tex, pal_uv) * vec4(1, 1, 1, data.y);\n"
                "}\n";
            display_vs_src =
                "#version 330\n"
                "layout(location=0) in vec4 pos;\n"
                "out vec2 uv;\n"
                "void main() {\n"
                "  gl_Position = vec4((pos.xy - 0.5) * 2.0, 0.0, 1.0);\n"
                "  uv = pos.xy;\n"
                "}\n";
            display_fs_src =
                "#version 330\n"
                "uniform sampler2D tex;\n"
                "in vec2 uv;\n"
                "out vec4 frag_color;\n"
                "void main() {\n"
                "  frag_color = texture(tex, uv);\n"
                "}\n";
                break;
        case SG_BACKEND_GLES3:
            offscreen_vs_src =
                "attribute vec4 pos;\n"
                "attribute vec2 uv_in;\n"
                "attribute vec4 data_in;\n"
                "varying vec2 uv;\n"
                "varying vec4 data;\n"
                "void main() {\n"
                "  gl_Position = vec4((pos.xy - 0.5) * vec2(2.0, -2.0), 0.5, 1.0);\n"
                "  uv  = uv_in;"
                "  data = data_in;\n"
                "}\n";
            offscreen_fs_src =
                "precision mediump float;\n"
                "uniform sampler2D tile_tex;\n"
                "uniform sampler2D pal_tex;\n"
                "varying vec2 uv;\n"
                "varying vec4 data;\n"
                "void main() {\n"
                "  float color_code = data.x;\n"
                "  float tile_color = texture2D(tile_tex, uv).x;\n"
                "  vec2 pal_uv = vec2(color_code * 4.0 + tile_color, 0.0);\n"
                "  gl_FragColor = texture2D(pal_tex, pal_uv) * vec4(1.0, 1.0, 1.0, data.y);\n"
                "}\n";
            display_vs_src =
                "attribute vec4 pos;\n"
                "varying vec2 uv;\n"
                "void main() {\n"
                "  gl_Position = vec4((pos.xy - 0.5) * 2.0, 0.0, 1.0);\n"
                "  uv = pos.xy;\n"
                "}\n";
            display_fs_src =
                "precision mediump float;\n"
                "uniform sampler2D tex;\n"
                "varying vec2 uv;\n"
                "void main() {\n"
                "  gl_FragColor = texture2D(tex, uv);\n"
                "}\n";
                break;
        default:
            assert(false);
    }

    // create pipeline and shader object for rendering into offscreen render target
    state.gfx.offscreen.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(&(sg_shader_desc){
           .attrs = {
                [0] = { .name="pos", .sem_name="POSITION" },
                [1] = { .name="uv_in", .sem_name="TEXCOORD", .sem_index=0 },
                [2] = { .name="data_in", .sem_name="TEXCOORD", .sem_index=1 },
            },
            .vs.source = offscreen_vs_src,
            .fs = {
                .images = {
                    [0] = { .used = true },
                    [1] = { .used = true },
                },
                .samplers = {
                    [0] = { .used = true },
                    [1] = { .used = true },
                },
                .image_sampler_pairs = {
                    [0] = { .used = true, .image_slot = 0, .sampler_slot = 0, .glsl_name = "tile_tex" },
                    [1] = { .used = true, .image_slot = 1, .sampler_slot = 1, .glsl_name = "pal_tex" },
                },
                .source = offscreen_fs_src
            }
        }),
        .layout = {
            .attrs = {
                [0].format = SG_VERTEXFORMAT_FLOAT2,
                [1].format = SG_VERTEXFORMAT_FLOAT2,
                [2].format = SG_VERTEXFORMAT_UBYTE4N,
            }
        },
        .depth.pixel_format = SG_PIXELFORMAT_NONE,
        .colors[0] = {
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            }
        }
    });

    // create pipeline and shader for rendering into display
    state.gfx.display.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(&(sg_shader_desc){
            .attrs[0] = { .name="pos", .sem_name="POSITION" },
            .vs.source = display_vs_src,
            .fs = {
                .images[0].used = true,
                .samplers[0].used = true,
                .image_sampler_pairs[0] = { .used = true, .image_slot = 0, .sampler_slot = 0, .glsl_name = "tex" },
                .source = display_fs_src
            }
        }),
        .layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2,
        .primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP
    });

    // create a render target image with a fixed upscale ratio
    state.gfx.offscreen.render_target = sg_make_image(&(sg_image_desc){
        .render_target = true,
        .width = DISPLAY_PIXELS_X * 2,
        .height = DISPLAY_PIXELS_Y * 2,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
    });

    // create an sampler to render the offscreen render target with linear upscale filtering
    state.gfx.display.sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

    // pass object for rendering into the offscreen render target
    state.gfx.offscreen.pass = sg_make_pass(&(sg_pass_desc){
        .color_attachments[0].image = state.gfx.offscreen.render_target
    });

    // create the 'tile-ROM-texture'
    state.gfx.offscreen.tile_img = sg_make_image(&(sg_image_desc){
        .width  = TILE_TEXTURE_WIDTH,
        .height = TILE_TEXTURE_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_R8,
        .data.subimage[0][0] = SG_RANGE(state.gfx.tile_pixels)
    });

    // create the palette texture
    state.gfx.offscreen.palette_img = sg_make_image(&(sg_image_desc){
        .width = 256,
        .height = 1,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.subimage[0][0] = SG_RANGE(state.gfx.color_palette)
    });

    // create a sampler with nearest filtering for the offscreen pass
    state.gfx.offscreen.sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
}
///////////////////////////////////GFX//////////////////////////////////////////////////
/*
    8x4 tile decoder (taken from: https://github.com/floooh/chips/blob/master/systems/namco.h)

    This decodes 2-bit-per-pixel tile data from Pacman ROM dumps into
    8-bit-per-pixel texture data (without doing the RGB palette lookup,
    this happens during rendering in the pixel shader).

    The Pacman ROM tile layout isn't exactly strightforward, both 8x8 tiles
    and 16x16 sprites are built from 8x4 pixel blocks layed out linearly
    in memory, and to add to the confusion, since Pacman is an arcade machine
    with the display 90 degree rotated, all the ROM tile data is counter-rotated.

    Tile decoding only happens once at startup from ROM dumps into a texture.
*/
static inline void gfx_decode_tile_8x4(
    uint32_t tex_x,
    uint32_t tex_y,
    const uint8_t* tile_base,
    uint32_t tile_stride,
    uint32_t tile_offset,
    uint8_t tile_code)
{
    for (uint32_t tx = 0; tx < TILE_WIDTH; tx++) {
        uint32_t ti = tile_code * tile_stride + tile_offset + (7 - tx);
        for (uint32_t ty = 0; ty < (TILE_HEIGHT/2); ty++) {
            uint8_t p_hi = (tile_base[ti] >> (7 - ty)) & 1;
            uint8_t p_lo = (tile_base[ti] >> (3 - ty)) & 1;
            uint8_t p = (p_hi << 1) | p_lo;
            state.gfx.tile_pixels[tex_y + ty][tex_x + tx] = p;
        }
    }
}

// decode an 8x8 tile into the tile texture's upper half
static inline void gfx_decode_tile(uint8_t tile_code) {
    uint32_t x = tile_code * TILE_WIDTH;
    uint32_t y0 = 0;
    uint32_t y1 = y0 + (TILE_HEIGHT / 2);
    gfx_decode_tile_8x4(x, y0, rom_tiles, 16, 8, tile_code);
    gfx_decode_tile_8x4(x, y1, rom_tiles, 16, 0, tile_code);
}

// decode a 16x16 sprite into the tile texture's lower half
static inline void gfx_decode_sprite(uint8_t sprite_code) {
    uint32_t x0 = sprite_code * SPRITE_WIDTH;
    uint32_t x1 = x0 + TILE_WIDTH;
    uint32_t y0 = TILE_HEIGHT;
    uint32_t y1 = y0 + (TILE_HEIGHT / 2);
    uint32_t y2 = y1 + (TILE_HEIGHT / 2);
    uint32_t y3 = y2 + (TILE_HEIGHT / 2);
    gfx_decode_tile_8x4(x0, y0, rom_sprites, 64, 40, sprite_code);
    gfx_decode_tile_8x4(x1, y0, rom_sprites, 64,  8, sprite_code);
    gfx_decode_tile_8x4(x0, y1, rom_sprites, 64, 48, sprite_code);
    gfx_decode_tile_8x4(x1, y1, rom_sprites, 64, 16, sprite_code);
    gfx_decode_tile_8x4(x0, y2, rom_sprites, 64, 56, sprite_code);
    gfx_decode_tile_8x4(x1, y2, rom_sprites, 64, 24, sprite_code);
    gfx_decode_tile_8x4(x0, y3, rom_sprites, 64, 32, sprite_code);
    gfx_decode_tile_8x4(x1, y3, rom_sprites, 64,  0, sprite_code);
}

// decode the Pacman tile- and sprite-ROM-dumps into a 8bpp texture
static void gfx_decode_tiles(void) {
    for (uint32_t tile_code = 0; tile_code < 256; tile_code++) {
        gfx_decode_tile(tile_code);
    }
    for (uint32_t sprite_code = 0; sprite_code < 64; sprite_code++) {
        gfx_decode_sprite(sprite_code);
    }
    // write a special opaque 16x16 block which will be used for the fade-effect
    for (uint32_t y = TILE_HEIGHT; y < TILE_TEXTURE_HEIGHT; y++) {
        for (uint32_t x = 64*SPRITE_WIDTH; x < 65*SPRITE_WIDTH; x++) {
            state.gfx.tile_pixels[y][x] = 1;
        }
    }
}

/* decode the Pacman color palette into a palette texture, on the original
    hardware, color lookup happens in two steps, first through 256-entry
    palette which indirects into a 32-entry hardware-color palette
    (of which only 16 entries are used on the Pacman hardware)
*/
static void gfx_decode_color_palette(void) {
    uint32_t hw_colors[32];
    for (int i = 0; i < 32; i++) {
       /*
           Each color ROM entry describes an RGB color in 1 byte:

           | 7| 6| 5| 4| 3| 2| 1| 0|
           |B1|B0|G2|G1|G0|R2|R1|R0|

           Intensities are: 0x97 + 0x47 + 0x21
        */
        uint8_t rgb = rom_hwcolors[i];
        uint8_t r = ((rgb>>0)&1) * 0x21 + ((rgb>>1)&1) * 0x47 + ((rgb>>2)&1) * 0x97;
        uint8_t g = ((rgb>>3)&1) * 0x21 + ((rgb>>4)&1) * 0x47 + ((rgb>>5)&1) * 0x97;
        uint8_t b = ((rgb>>6)&1) * 0x47 + ((rgb>>7)&1) * 0x97;
        hw_colors[i] = 0xFF000000 | (b<<16) | (g<<8) | r;
    }
    for (int i = 0; i < 256; i++) {
        state.gfx.color_palette[i] = hw_colors[rom_palette[i] & 0xF];
        // first color in each color block is transparent
        if ((i & 3) == 0) {
            state.gfx.color_palette[i] &= 0x00FFFFFF;
        }
    }
}

static void gfx_init(void) {
    sg_setup(&(sg_desc){
        // reduce pool allocation size to what's actually needed
        .buffer_pool_size = 2,
        .image_pool_size = 3,
        .shader_pool_size = 2,
        .pipeline_pool_size = 2,
        .pass_pool_size = 1,
        .context = sapp_sgcontext(),
        .logger.func = slog_func,
    });
    disable(&state.gfx.fadein);
    disable(&state.gfx.fadeout);
    state.gfx.fade = 0xFF;
    spr_clear();
    gfx_decode_tiles();
    gfx_decode_color_palette();
    gfx_create_resources();
}

static void gfx_shutdown(void) {
    sg_shutdown();
}

static void gfx_add_vertex(float x, float y, float u, float v, uint8_t color_code, uint8_t opacity) {
    assert(state.gfx.num_vertices < MAX_VERTICES);
    vertex_t* vtx = &state.gfx.vertices[state.gfx.num_vertices++];
    vtx->x = x;
    vtx->y = y;
    vtx->u = u;
    vtx->v = v;
    vtx->attr = (opacity<<8) | color_code;
}

static void gfx_add_tile_vertices(uint32_t tx, uint32_t ty, uint8_t tile_code, uint8_t color_code) {
    assert((tx < DISPLAY_TILES_X) && (ty < DISPLAY_TILES_Y));
    const float dx = 1.0f / DISPLAY_TILES_X;
    const float dy = 1.0f / DISPLAY_TILES_Y;
    const float du = (float)TILE_WIDTH / TILE_TEXTURE_WIDTH;
    const float dv = (float)TILE_HEIGHT / TILE_TEXTURE_HEIGHT;

    const float x0 = tx * dx;
    const float x1 = x0 + dx;
    const float y0 = ty * dy;
    const float y1 = y0 + dy;
    const float u0 = tile_code * du;
    const float u1 = u0 + du;
    const float v0 = 0.0f;
    const float v1 = dv;
    /*
        x0,y0
        +-----+
        | *   |
        |   * |
        +-----+
                x1,y1
    */
    gfx_add_vertex(x0, y0, u0, v0, color_code, 0xFF);
    gfx_add_vertex(x1, y0, u1, v0, color_code, 0xFF);
    gfx_add_vertex(x1, y1, u1, v1, color_code, 0xFF);
    gfx_add_vertex(x0, y0, u0, v0, color_code, 0xFF);
    gfx_add_vertex(x1, y1, u1, v1, color_code, 0xFF);
    gfx_add_vertex(x0, y1, u0, v1, color_code, 0xFF);
}

static void gfx_add_playfield_vertices(void) {
    for (uint32_t ty = 0; ty < DISPLAY_TILES_Y; ty++) {
        for (uint32_t tx = 0; tx < DISPLAY_TILES_X; tx++) {
            const uint8_t tile_code = state.gfx.video_ram[ty][tx];
            const uint8_t color_code = state.gfx.color_ram[ty][tx] & 0x1F;
            gfx_add_tile_vertices(tx, ty, tile_code, color_code);
        }
    }
}

static void gfx_add_debugmarker_vertices(void) {
    for (int i = 0; i < NUM_DEBUG_MARKERS; i++) {
        const debugmarker_t* dbg = &state.gfx.debug_marker[i];
        if (dbg->enabled) {
            gfx_add_tile_vertices(dbg->tile_pos.x, dbg->tile_pos.y, dbg->tile, dbg->color);
        }
    }
}

static void gfx_add_sprite_vertices(void) {
    const float dx = 1.0f / DISPLAY_PIXELS_X;
    const float dy = 1.0f / DISPLAY_PIXELS_Y;
    const float du = (float)SPRITE_WIDTH / TILE_TEXTURE_WIDTH;
    const float dv = (float)SPRITE_HEIGHT / TILE_TEXTURE_HEIGHT;
    for (int i = 0; i < NUM_SPRITES; i++) {
        const sprite_t* spr = &state.gfx.sprite[i];
        if (spr->enabled) {
            float x0, x1, y0, y1;
            if (spr->flipx) {
                x1 = spr->pos.x * dx;
                x0 = x1 + dx * SPRITE_WIDTH;
            }
            else {
                x0 = spr->pos.x * dx;
                x1 = x0 + dx * SPRITE_WIDTH;
            }
            if (spr->flipy) {
                y1 = spr->pos.y * dy;
                y0 = y1 + dy * SPRITE_HEIGHT;
            }
            else {
                y0 = spr->pos.y * dy;
                y1 = y0 + dy * SPRITE_HEIGHT;
            }
            const float u0 = spr->tile * du;
            const float u1 = u0 + du;
            const float v0 = ((float)TILE_HEIGHT / TILE_TEXTURE_HEIGHT);
            const float v1 = v0 + dv;
            const uint8_t color = spr->color;
            gfx_add_vertex(x0, y0, u0, v0, color, 0xFF);
            gfx_add_vertex(x1, y0, u1, v0, color, 0xFF);
            gfx_add_vertex(x1, y1, u1, v1, color, 0xFF);
            gfx_add_vertex(x0, y0, u0, v0, color, 0xFF);
            gfx_add_vertex(x1, y1, u1, v1, color, 0xFF);
            gfx_add_vertex(x0, y1, u0, v1, color, 0xFF);
        }
    }
}

static void gfx_add_fade_vertices(void) {
    // sprite tile 64 is a special 16x16 opaque block
    const float du = (float)SPRITE_WIDTH / TILE_TEXTURE_WIDTH;
    const float dv = (float)SPRITE_HEIGHT / TILE_TEXTURE_HEIGHT;
    const float u0 = 64 * du;
    const float u1 = u0 + du;
    const float v0 = (float)TILE_HEIGHT / TILE_TEXTURE_HEIGHT;
    const float v1 = v0 + dv;

    const uint8_t fade = state.gfx.fade;
    gfx_add_vertex(0.0f, 0.0f, u0, v0, 0, fade);
    gfx_add_vertex(1.0f, 0.0f, u1, v0, 0, fade);
    gfx_add_vertex(1.0f, 1.0f, u1, v1, 0, fade);
    gfx_add_vertex(0.0f, 0.0f, u0, v0, 0, fade);
    gfx_add_vertex(1.0f, 1.0f, u1, v1, 0, fade);
    gfx_add_vertex(0.0f, 1.0f, u0, v1, 0, fade);
}

// adjust the viewport so that the aspect ratio is always correct
static void gfx_adjust_viewport(int canvas_width, int canvas_height) {
    const float canvas_aspect = (float)canvas_width / (float)canvas_height;
    const float playfield_aspect = (float)DISPLAY_TILES_X / (float)DISPLAY_TILES_Y;
    int vp_x, vp_y, vp_w, vp_h;
    const int border = 10;
    if (playfield_aspect < canvas_aspect) {
        vp_y = border;
        vp_h = canvas_height - 2*border;
        vp_w = (int)(canvas_height * playfield_aspect - 2*border);
        vp_x = (canvas_width - vp_w) / 2;
    }
    else {
        vp_x = border;
        vp_w = canvas_width - 2*border;
        vp_h = (int)(canvas_width / playfield_aspect - 2*border);
        vp_y = (canvas_height - vp_h) / 2;
    }
    sg_apply_viewport(vp_x, vp_y, vp_w, vp_h, true);
}

// handle fadein/fadeout
static void gfx_fade(void) {
    if (between(state.gfx.fadein, 0, FADE_TICKS)) {
        float t = (float)since(state.gfx.fadein) / FADE_TICKS;
        state.gfx.fade = (uint8_t) (255.0f * (1.0f - t));
    }
    if (after_once(state.gfx.fadein, FADE_TICKS)) {
        state.gfx.fade = 0;
    }
    if (between(state.gfx.fadeout, 0, FADE_TICKS)) {
        float t = (float)since(state.gfx.fadeout) / FADE_TICKS;
        state.gfx.fade = (uint8_t) (255.0f * t);
    }
    if (after_once(state.gfx.fadeout, FADE_TICKS)) {
        state.gfx.fade = 255;
    }
}

static void gfx_draw(void) {
    // handle fade in/out
    gfx_fade();

    // update the playfield and sprite vertex buffer
    state.gfx.num_vertices = 0;
    gfx_add_playfield_vertices();
    gfx_add_sprite_vertices();
    gfx_add_debugmarker_vertices();
    if (state.gfx.fade > 0) {
        gfx_add_fade_vertices();
    }
    assert(state.gfx.num_vertices <= MAX_VERTICES);
    sg_update_buffer(state.gfx.offscreen.vbuf, &(sg_range){ .ptr=state.gfx.vertices, .size=state.gfx.num_vertices * sizeof(vertex_t) });

    // render tiles and sprites into offscreen render target
    sg_begin_pass(state.gfx.offscreen.pass, &state.gfx.pass_action);
    sg_apply_pipeline(state.gfx.offscreen.pip);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = state.gfx.offscreen.vbuf,
        .fs = {
            .images = {
                [0] = state.gfx.offscreen.tile_img,
                [1] = state.gfx.offscreen.palette_img,
            },
            .samplers[0] = state.gfx.offscreen.sampler,
            .samplers[1] = state.gfx.offscreen.sampler,
        }
    });
    sg_draw(0, state.gfx.num_vertices, 1);
    sg_end_pass();

    // upscale-render the offscreen render target into the display framebuffer
    const int canvas_width = sapp_width();
    const int canvas_height = sapp_height();
    sg_begin_default_pass(&state.gfx.pass_action, canvas_width, canvas_height);
    gfx_adjust_viewport(canvas_width, canvas_height);
    sg_apply_pipeline(state.gfx.display.pip);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = state.gfx.display.quad_vbuf,
        .fs = {
            .images[0] = state.gfx.offscreen.render_target,
            .samplers[0] = state.gfx.display.sampler,
        }
    });
    sg_draw(0, 4, 1);
    sg_end_pass();
    sg_commit();
}
//////////////////////////////////////////AUDIO/////////////////////////////////////////////
/*== AUDIO SUBSYSTEM =========================================================*/
static void snd_init(void) {
    saudio_setup(&(saudio_desc){
        .logger.func = slog_func,
    });

    // compute sample duration in nanoseconds
    int32_t samples_per_sec = saudio_sample_rate();
    state.audio.sample_duration_ns = 1000000000 / samples_per_sec;

    /* compute number of 96kHz ticks per sample tick (the Namco sound generator
        runs at 96kHz), times 1000 for increased precision
    */
    state.audio.voice_tick_period = 96000000 / samples_per_sec;
}

static void snd_shutdown(void) {
    saudio_shutdown();
}

// the snd_voice_tick() function updates the Namco sound generator and must be called with 96 kHz
static void snd_voice_tick(void) {
    for (int i = 0; i < NUM_VOICES; i++) {
        voice_t* voice = &state.audio.voice[i];
        voice->counter += voice->frequency;
        /* lookup current 4-bit sample from the waveform number and the
            topmost 5 bits of the 20-bit sample counter
        */
        uint32_t wave_index = ((voice->waveform<<5) | ((voice->counter>>15) & 0x1F)) & 0xFF;
        int sample = (((int)(rom_wavetable[wave_index] & 0xF)) - 8) * voice->volume;
        voice->sample_acc += (float)sample; // sample is (-8..+7 wavetable value) * 16 (volume)
        voice->sample_div += 128.0f;
    }
}

// the snd_sample_tick() function must be called with sample frequency (e.g. 44.1kHz)
static void snd_sample_tick(void) {
    float sm = 0.0f;
    for (int i = 0; i < NUM_VOICES; i++) {
        voice_t* voice = &state.audio.voice[i];
        if (voice->sample_div > 0.0f) {
            sm += voice->sample_acc / voice->sample_div;
            voice->sample_acc = voice->sample_div = 0.0f;
        }
    }
    state.audio.sample_buffer[state.audio.num_samples++] = sm * 0.333333f * AUDIO_VOLUME;
    if (state.audio.num_samples == NUM_SAMPLES) {
        saudio_push(state.audio.sample_buffer, state.audio.num_samples);
        state.audio.num_samples = 0;
    }
}

// the sound subsystem's per-frame function
static void snd_frame(int32_t frame_time_ns) {
    // for each sample to generate...
    state.audio.sample_accum -= frame_time_ns;
    while (state.audio.sample_accum < 0) {
        state.audio.sample_accum += state.audio.sample_duration_ns;
        // tick the sound generator at 96 KHz
        state.audio.voice_tick_accum -= state.audio.voice_tick_period;
        while (state.audio.voice_tick_accum < 0) {
            state.audio.voice_tick_accum += 1000;
            snd_voice_tick();
        }
        // generate a new sample, and push out to sokol-audio when local sample buffer full
        snd_sample_tick();
    }
}

/* The sound system's 60 Hz tick function (called from game tick).
    Updates the sound 'hardware registers' for all active sound effects.
*/
static void snd_tick(void) {
    // for each active sound effect...
    for (int sound_slot = 0; sound_slot < NUM_SOUNDS; sound_slot++) {
        sound_t* snd = &state.audio.sound[sound_slot];
        if (snd->func) {
            // procedural sound effect
            snd->func(sound_slot);
        }
        else if (snd->flags & SOUNDFLAG_ALL_VOICES) {
            // register-dump sound effect
            assert(snd->data);
            if (snd->cur_tick == snd->num_ticks) {
                snd_stop(sound_slot);
                continue;
            }

            // decode register dump values into voice 'registers'
            const uint32_t* cur_ptr = &snd->data[snd->cur_tick * snd->stride];
            for (int i = 0; i < NUM_VOICES; i++) {
                if (snd->flags & (1<<i)) {
                    voice_t* voice = &state.audio.voice[i];
                    uint32_t val = *cur_ptr++;
                    // 20 bits frequency
                    voice->frequency = val & ((1<<20)-1);
                    // 3 bits waveform
                    voice->waveform = (val>>24) & 7;
                    // 4 bits volume
                    voice->volume = (val>>28) & 0xF;
                }
            }
        }
        snd->cur_tick++;
    }
}

// clear all active sound effects and start outputting silence
static void snd_clear(void) {
    memset(&state.audio.voice, 0, sizeof(state.audio.voice));
    memset(&state.audio.sound, 0, sizeof(state.audio.sound));
}

// start a sound effect
static void snd_start(int slot, const sound_desc_t* desc) {
    assert((slot >= 0) && (slot < NUM_SOUNDS));
    assert(desc);
    assert((desc->ptr && desc->size) || desc->func);

    sound_t* snd = &state.audio.sound[slot];
    *snd = (sound_t) { 0 };
    int num_voices = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (desc->voice[i]) {
            snd->flags |= (1<<i);
            num_voices++;
        }
    }
    if (desc->func) {
        // procedural sounds only need a callback function
        snd->func = desc->func;
    }
    else {
        assert(num_voices > 0);
        assert((desc->size % (num_voices*sizeof(uint32_t))) == 0);
        snd->stride = num_voices;
        snd->num_ticks = desc->size / (snd->stride*sizeof(uint32_t));
        snd->data = desc->ptr;
    }
}

// stop a sound effect
static void snd_stop(int slot) {
    assert((slot >= 0) && (slot < NUM_SOUNDS));

    // silence the sound's output voices
    for (int i = 0; i < NUM_VOICES; i++) {
        if (state.audio.sound[slot].flags & (1<<i)) {
            state.audio.voice[i] = (voice_t) { 0 };
        }
    }

    // clear the sound slot
    state.audio.sound[slot] = (sound_t) { 0 };
}

// procedural sound effects
static void snd_func_eatdot1(int slot) {
    assert((slot >= 0) && (slot < NUM_SOUNDS));
    const sound_t* snd = &state.audio.sound[slot];
    voice_t* voice = &state.audio.voice[2];
    if (snd->cur_tick == 0) {
        voice->volume = 12;
        voice->waveform = 2;
        voice->frequency = 0x1500;
    }
    else if (snd->cur_tick == 5) {
        snd_stop(slot);
    }
    else {
        voice->frequency -= 0x0300;
    }
}

static void snd_func_eatdot2(int slot) {
    assert((slot >= 0) && (slot < NUM_SOUNDS));
    const sound_t* snd = &state.audio.sound[slot];
    voice_t* voice = &state.audio.voice[2];
    if (snd->cur_tick == 0) {
        voice->volume = 12;
        voice->waveform = 2;
        voice->frequency = 0x0700;
    }
    else if (snd->cur_tick == 5) {
        snd_stop(slot);
    }
    else {
        voice->frequency += 0x300;
    }
}

static void snd_func_eatghost(int slot) {
    assert((slot >= 0) && (slot < NUM_SOUNDS));
    const sound_t* snd = &state.audio.sound[slot];
    voice_t* voice = &state.audio.voice[2];
    if (snd->cur_tick == 0) {
        voice->volume = 12;
        voice->waveform = 5;
        voice->frequency = 0;
    }
    else if (snd->cur_tick == 32) {
        snd_stop(slot);
    }
    else {
        voice->frequency += 0x20;
    }
}

static void snd_func_eatfruit(int slot) {
    assert((slot >= 0) && (slot < NUM_SOUNDS));
    const sound_t* snd = &state.audio.sound[slot];
    voice_t* voice = &state.audio.voice[2];
    if (snd->cur_tick == 0) {
        voice->volume = 15;
        voice->waveform = 6;
        voice->frequency = 0x1600;
    }
    else if (snd->cur_tick == 23) {
        snd_stop(slot);
    }
    else if (snd->cur_tick < 11) {
        voice->frequency -= 0x200;
    }
    else {
        voice->frequency += 0x0200;
    }
}

static void snd_func_weeooh(int slot) {
    assert((slot >= 0) && (slot < NUM_SOUNDS));
    const sound_t* snd = &state.audio.sound[slot];
    voice_t* voice = &state.audio.voice[1];
    if (snd->cur_tick == 0) {
        voice->volume = 6;
        voice->waveform = 6;
        voice->frequency = 0x1000;
    }
    else if ((snd->cur_tick % 24) < 12) {
        voice->frequency += 0x0200;
    }
    else {
        voice->frequency -= 0x0200;
    }
}

static void snd_func_frightened(int slot) {
    assert((slot >= 0) && (slot < NUM_SOUNDS));
    const sound_t* snd = &state.audio.sound[slot];
    voice_t* voice = &state.audio.voice[1];
    if (snd->cur_tick == 0) {
        voice->volume = 10;
        voice->waveform = 4;
        voice->frequency = 0x0180;
    }
    else if ((snd->cur_tick % 8) == 0) {
        voice->frequency = 0x0180;
    }
    else {
        voice->frequency += 0x180;
    }
}

/*== EMBEDDED DATA ===========================================================*/

// Pacman sprite ROM dump
static const uint8_t rom_tiles[4096] = {
    0xcc, 0xee, 0x11, 0x11, 0x33, 0xee, 0xcc, 0x0, 0x11, 0x33, 0x66, 0x44, 0x44, 0x33, 0x11, 0x0,
    0x11, 0x11, 0xff, 0xff, 0x11, 0x11, 0x0, 0x0, 0x0, 0x0, 0x77, 0x77, 0x22, 0x0, 0x0, 0x0,
    0x11, 0x99, 0xdd, 0xdd, 0xff, 0x77, 0x33, 0x0, 0x33, 0x77, 0x55, 0x44, 0x44, 0x66, 0x22, 0x0,
    0x66, 0xff, 0x99, 0x99, 0x99, 0x33, 0x22, 0x0, 0x44, 0x66, 0x77, 0x55, 0x44, 0x44, 0x0, 0x0,
    0x44, 0xff, 0xff, 0x44, 0x44, 0xcc, 0xcc, 0x0, 0x0, 0x77, 0x77, 0x66, 0x33, 0x11, 0x0, 0x0,
    0xee, 0xff, 0x11, 0x11, 0x11, 0x33, 0x22, 0x0, 0x0, 0x55, 0x55, 0x55, 0x55, 0x77, 0x77, 0x0,
    0x66, 0xff, 0x99, 0x99, 0x99, 0xff, 0xee, 0x0, 0x0, 0x44, 0x44, 0x44, 0x66, 0x33, 0x11, 0x0,
    0x0, 0x0, 0x88, 0xff, 0x77, 0x0, 0x0, 0x0, 0x66, 0x77, 0x55, 0x44, 0x44, 0x66, 0x66, 0x0,
    0x66, 0x77, 0xdd, 0xdd, 0x99, 0x99, 0x66, 0x0, 0x0, 0x33, 0x44, 0x44, 0x55, 0x77, 0x33, 0x0,
    0xcc, 0xee, 0xbb, 0x99, 0x99, 0x99, 0x0, 0x0, 0x33, 0x77, 0x44, 0x44, 0x44, 0x77, 0x33, 0x0,
    0xff, 0xff, 0x44, 0x44, 0x44, 0xff, 0xff, 0x0, 0x11, 0x33, 0x66, 0x44, 0x66, 0x33, 0x11, 0x0,
    0x66, 0xff, 0x99, 0x99, 0x99, 0xff, 0xff, 0x0, 0x33, 0x77, 0x44, 0x44, 0x44, 0x77, 0x77, 0x0,
    0x22, 0x33, 0x11, 0x11, 0x33, 0xee, 0xcc, 0x0, 0x22, 0x66, 0x44, 0x44, 0x66, 0x33, 0x11, 0x0,
    0xcc, 0xee, 0x33, 0x11, 0x11, 0xff, 0xff, 0x0, 0x11, 0x33, 0x66, 0x44, 0x44, 0x77, 0x77, 0x0,
    0x11, 0x99, 0x99, 0x99, 0xff, 0xff, 0x0, 0x0, 0x44, 0x44, 0x44, 0x44, 0x77, 0x77, 0x0, 0x0,
    0x0, 0x88, 0x88, 0x88, 0x88, 0xff, 0xff, 0x0, 0x44, 0x44, 0x44, 0x44, 0x44, 0x77, 0x77, 0x0,
    0x0, 0x0, 0x0, 0x8, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x1, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x8, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x1, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x8, 0xc, 0xc, 0x8, 0x0, 0x0, 0x0, 0x0, 0x1, 0x3, 0x3, 0x1, 0x0, 0x0,
    0x0, 0x0, 0x8, 0xc, 0xc, 0x8, 0x0, 0x0, 0x0, 0x0, 0x1, 0x3, 0x3, 0x1, 0x0, 0x0,
    0xc, 0xe, 0xf, 0xf, 0xf, 0xf, 0xe, 0xc, 0x3, 0x7, 0xf, 0xf, 0xf, 0xf, 0x7, 0x3,
    0xc, 0xe, 0xf, 0xf, 0xf, 0xf, 0xe, 0xc, 0x3, 0x7, 0xf, 0xf, 0xf, 0xf, 0x7, 0x3,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x77, 0xff, 0xff, 0xff, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33,
    0xee, 0xcc, 0xcc, 0x88, 0x88, 0x0, 0x0, 0x0, 0x33, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0x88, 0x0, 0x0, 0x0, 0xcc, 0xee, 0xff, 0xff, 0xff,
    0x88, 0x88, 0x88, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x77, 0x77, 0x33, 0x22, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x66, 0x66, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xdd, 0x0, 0xee, 0xdd, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xdd, 0x0, 0xee, 0xdd, 0x0, 0x0,
    0xff, 0xff, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0x77, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xff, 0xff,
    0xbb, 0xbb, 0xbb, 0xbb, 0xff, 0xff, 0x0, 0x0, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0x11, 0x0, 0x0,
    0x0, 0x0, 0xff, 0xff, 0x0, 0x0, 0xff, 0xff, 0xcc, 0xcc, 0xff, 0xff, 0x0, 0x0, 0x77, 0xff,
    0x0, 0x0, 0xff, 0xff, 0x0, 0x0, 0xff, 0xff, 0x0, 0x0, 0x77, 0xff, 0xcc, 0xcc, 0xff, 0xff,
    0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0xff, 0xee, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xff, 0x77,
    0x33, 0x33, 0x33, 0x33, 0xff, 0xee, 0x0, 0x0, 0xcc, 0xcc, 0xcc, 0xcc, 0xff, 0x77, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x77, 0xff,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x88, 0xcc, 0x22, 0x22, 0x66, 0xcc, 0x88, 0x0, 0x33, 0x77, 0xcc, 0x88, 0x88, 0x77, 0x33, 0x0,
    0x22, 0x22, 0xee, 0xee, 0x22, 0x22, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0x44, 0x0, 0x0, 0x0,
    0x22, 0x22, 0xaa, 0xaa, 0xee, 0xee, 0x66, 0x0, 0x66, 0xff, 0xbb, 0x99, 0x99, 0xcc, 0x44, 0x0,
    0xcc, 0xee, 0x22, 0x22, 0x22, 0x66, 0x44, 0x0, 0x88, 0xdd, 0xff, 0xbb, 0x99, 0x88, 0x0, 0x0,
    0x88, 0xee, 0xee, 0x88, 0x88, 0x88, 0x88, 0x0, 0x0, 0xff, 0xff, 0xcc, 0x66, 0x33, 0x11, 0x0,
    0xcc, 0xee, 0x22, 0x22, 0x22, 0x66, 0x44, 0x0, 0x11, 0xbb, 0xaa, 0xaa, 0xaa, 0xee, 0xee, 0x0,
    0xcc, 0xee, 0x22, 0x22, 0x22, 0xee, 0xcc, 0x0, 0x0, 0x99, 0x99, 0x99, 0xdd, 0x77, 0x33, 0x0,
    0x0, 0x0, 0x0, 0xee, 0xee, 0x0, 0x0, 0x0, 0xcc, 0xee, 0xbb, 0x99, 0x88, 0xcc, 0xcc, 0x0,
    0xcc, 0xee, 0xaa, 0xaa, 0x22, 0x22, 0xcc, 0x0, 0x0, 0x66, 0x99, 0x99, 0xbb, 0xff, 0x66, 0x0,
    0x88, 0xcc, 0x66, 0x22, 0x22, 0x22, 0x0, 0x0, 0x77, 0xff, 0x99, 0x99, 0x99, 0xff, 0x66, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x88, 0x44, 0x22, 0x0, 0x88, 0x44, 0x22, 0x11, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x11, 0x11, 0x0, 0x0, 0x0,
    0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
    0xff, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0xff,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xee, 0xee, 0x88, 0x88, 0x88, 0xee, 0xee, 0x0, 0x33, 0x77, 0xcc, 0x88, 0xcc, 0x77, 0x33, 0x0,
    0xcc, 0xee, 0x22, 0x22, 0x22, 0xee, 0xee, 0x0, 0x66, 0xff, 0x99, 0x99, 0x99, 0xff, 0xff, 0x0,
    0x44, 0x66, 0x22, 0x22, 0x66, 0xcc, 0x88, 0x0, 0x44, 0xcc, 0x88, 0x88, 0xcc, 0x77, 0x33, 0x0,
    0x88, 0xcc, 0x66, 0x22, 0x22, 0xee, 0xee, 0x0, 0x33, 0x77, 0xcc, 0x88, 0x88, 0xff, 0xff, 0x0,
    0x22, 0x22, 0x22, 0x22, 0xee, 0xee, 0x0, 0x0, 0x88, 0x99, 0x99, 0x99, 0xff, 0xff, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xee, 0x0, 0x88, 0x99, 0x99, 0x99, 0x99, 0xff, 0xff, 0x0,
    0xee, 0xee, 0x22, 0x22, 0x66, 0xcc, 0x88, 0x0, 0x99, 0x99, 0x99, 0x88, 0xcc, 0x77, 0x33, 0x0,
    0xee, 0xee, 0x0, 0x0, 0x0, 0xee, 0xee, 0x0, 0xff, 0xff, 0x11, 0x11, 0x11, 0xff, 0xff, 0x0,
    0x22, 0x22, 0xee, 0xee, 0x22, 0x22, 0x0, 0x0, 0x88, 0x88, 0xff, 0xff, 0x88, 0x88, 0x0, 0x0,
    0xcc, 0xee, 0x22, 0x22, 0x22, 0x66, 0x44, 0x0, 0xff, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0x66, 0xee, 0xcc, 0x88, 0xee, 0xee, 0x0, 0x88, 0xcc, 0x66, 0x33, 0x11, 0xff, 0xff, 0x0,
    0x22, 0x22, 0x22, 0x22, 0xee, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0x0, 0x0,
    0xee, 0xee, 0x0, 0x88, 0x0, 0xee, 0xee, 0x0, 0xff, 0xff, 0x77, 0x33, 0x77, 0xff, 0xff, 0x0,
    0xee, 0xee, 0xcc, 0x88, 0x0, 0xee, 0xee, 0x0, 0xff, 0xff, 0x11, 0x33, 0x77, 0xff, 0xff, 0x0,
    0xcc, 0xee, 0x22, 0x22, 0x22, 0xee, 0xcc, 0x0, 0x77, 0xff, 0x88, 0x88, 0x88, 0xff, 0x77, 0x0,
    0x0, 0x88, 0x88, 0x88, 0x88, 0xee, 0xee, 0x0, 0x77, 0xff, 0x88, 0x88, 0x88, 0xff, 0xff, 0x0,
    0xaa, 0xcc, 0xee, 0xaa, 0x22, 0xee, 0xcc, 0x0, 0x77, 0xff, 0x88, 0x88, 0x88, 0xff, 0x77, 0x0,
    0x22, 0x66, 0xee, 0xcc, 0x88, 0xee, 0xee, 0x0, 0x77, 0xff, 0x99, 0x88, 0x88, 0xff, 0xff, 0x0,
    0xcc, 0xee, 0x22, 0x22, 0x22, 0x66, 0x44, 0x0, 0x0, 0x55, 0xdd, 0x99, 0x99, 0xff, 0x66, 0x0,
    0x0, 0x0, 0xee, 0xee, 0x0, 0x0, 0x0, 0x0, 0x88, 0x88, 0xff, 0xff, 0x88, 0x88, 0x0, 0x0,
    0xcc, 0xee, 0x22, 0x22, 0x22, 0xee, 0xcc, 0x0, 0xff, 0xff, 0x0, 0x0, 0x0, 0xff, 0xff, 0x0,
    0x0, 0x88, 0xcc, 0xee, 0xcc, 0x88, 0x0, 0x0, 0xff, 0xff, 0x11, 0x0, 0x11, 0xff, 0xff, 0x0,
    0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0x0, 0xff, 0xff, 0x11, 0x33, 0x11, 0xff, 0xff, 0x0,
    0x66, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x66, 0x0, 0xcc, 0xee, 0x77, 0x33, 0x77, 0xee, 0xcc, 0x0,
    0x0, 0x0, 0xee, 0xee, 0x0, 0x0, 0x0, 0x0, 0xee, 0xff, 0x11, 0x11, 0xff, 0xee, 0x0, 0x0,
    0x22, 0x22, 0x22, 0xaa, 0xee, 0xee, 0x66, 0x0, 0xcc, 0xee, 0xff, 0xbb, 0x99, 0x88, 0x88, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x88, 0x22, 0x0, 0x0, 0x0, 0xcc, 0xee, 0xff, 0x33, 0x0, 0x0, 0x0,
    0xcc, 0x22, 0x11, 0x55, 0x55, 0x99, 0x22, 0xcc, 0x33, 0x44, 0x88, 0xaa, 0xaa, 0x99, 0x44, 0x33,
    0x0, 0x0, 0x0, 0x0, 0x88, 0x88, 0x88, 0xee, 0x22, 0x22, 0x0, 0x11, 0x22, 0x22, 0x22, 0x33,
    0xaa, 0xaa, 0xaa, 0x22, 0x0, 0x0, 0x0, 0xee, 0x22, 0x22, 0x22, 0x11, 0x0, 0x22, 0x22, 0x33,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x44, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x22,
    0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xe, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x33, 0x77, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0, 0x0, 0x0, 0x0, 0x33, 0x33, 0x33, 0x33, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0x0,
    0x33, 0x33, 0x77, 0x77, 0xff, 0xff, 0xff, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
    0xff, 0xff, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x33, 0x33, 0x33, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xee, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x33, 0x33, 0x33, 0x33, 0x33, 0x77, 0x77, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x33, 0x33, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xee, 0xee, 0xee, 0xee, 0xee, 0x0, 0x0, 0x0,
    0x33, 0x11, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xce, 0xee, 0xee, 0xee, 0x66, 0x22, 0x22, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0xcc, 0x0, 0x22, 0xee, 0x22, 0x0, 0x0, 0x88, 0x77, 0x0, 0x0, 0xff, 0x44, 0x0, 0x0,
    0x22, 0xcc, 0x0, 0xcc, 0x22, 0x22, 0x22, 0x44, 0x88, 0x77, 0x0, 0x88, 0xdd, 0xaa, 0x88, 0x88,
    0x22, 0xcc, 0x0, 0xcc, 0x22, 0x22, 0x22, 0x44, 0x88, 0x77, 0x0, 0x99, 0xaa, 0xaa, 0xaa, 0xee,
    0x22, 0xcc, 0x0, 0x0, 0x0, 0xee, 0x0, 0x0, 0x88, 0x77, 0x0, 0xcc, 0xbb, 0x88, 0x88, 0xcc,
    0x0, 0xcc, 0x22, 0x22, 0xcc, 0x0, 0xcc, 0x22, 0x0, 0x77, 0x88, 0x88, 0x77, 0x0, 0x77, 0x88,
    0xcc, 0x22, 0x22, 0xcc, 0x0, 0x22, 0xee, 0x22, 0x77, 0x88, 0x88, 0x77, 0x0, 0x0, 0xff, 0x44,
    0x66, 0x22, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0x66, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xcc, 0x22, 0x22, 0xcc, 0x0, 0x22, 0x22, 0xaa, 0x77, 0x88, 0x88, 0x77, 0x0, 0x66, 0x99, 0x88,
    0x22, 0x44, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0x88, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xcc, 0x22, 0x22, 0xcc, 0x0, 0xcc, 0x22, 0x22, 0x77, 0x88, 0x88, 0x77, 0x0, 0x88, 0xdd, 0xaa,
    0x22, 0x44, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xaa, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xcc, 0x22, 0x22, 0xcc, 0x0, 0xcc, 0x22, 0x22, 0x77, 0x88, 0x88, 0x77, 0x0, 0x99, 0xaa, 0xaa,
    0x22, 0xcc, 0x0, 0xcc, 0x22, 0x22, 0xcc, 0x0, 0x88, 0x77, 0x0, 0x77, 0x88, 0x88, 0x77, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xcc, 0x22, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x77, 0x88,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0xc0, 0x20, 0x90, 0x80, 0x0, 0x0, 0x30, 0x30, 0x10, 0x10, 0x0, 0x0,
    0x41, 0x21, 0x12, 0x3, 0x3, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x8, 0xc, 0xc, 0x8c, 0xc, 0x0, 0x0, 0x0, 0x7, 0xf, 0xf, 0xc3, 0x1f,
    0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x7, 0x8, 0xf, 0x2f, 0x4f, 0xe, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x7, 0x4f, 0xf, 0xa7, 0x87, 0x0, 0x0, 0x0, 0x0, 0x0, 0x10, 0x10, 0x10,
    0xd3, 0x87, 0x97, 0xf, 0x2f, 0x7, 0x0, 0x0, 0x33, 0x10, 0x10, 0x10, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x8, 0x8, 0x0, 0x0, 0x0, 0x8, 0xe, 0x8e, 0x1f, 0xf,
    0xc, 0x8, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x4f, 0x1f, 0xf, 0x4f, 0xe, 0x8, 0x0, 0x0,
    0x0, 0x0, 0x1, 0x3, 0x87, 0x87, 0x87, 0x47, 0x0, 0x0, 0x0, 0x10, 0x10, 0x30, 0x30, 0x10,
    0xef, 0x47, 0x7, 0x7, 0x3, 0x1, 0x0, 0x0, 0x11, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x8, 0x8, 0xc, 0xc, 0xc, 0x0, 0x0, 0xe, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xc, 0xc, 0xc, 0x8, 0x8, 0x0, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xe, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x1, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x1, 0x2,
    0xf, 0xb, 0xc, 0xf, 0x1, 0x0, 0x0, 0x0, 0x2, 0x1, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0xc, 0x68, 0x68, 0x68, 0x6e, 0x6e, 0x0, 0x0, 0x3, 0xf, 0xf, 0xf, 0xf, 0xf,
    0x68, 0x68, 0x68, 0x68, 0x68, 0xc, 0x0, 0x0, 0xf, 0xf, 0x7, 0xc, 0xf, 0x3, 0x0, 0x0,
    0x0, 0x0, 0x7, 0xf, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x0, 0x0, 0x1, 0x1, 0x1, 0x20,
    0x87, 0xf, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x10, 0x0, 0x1, 0x1, 0x1, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x8, 0xc, 0xc, 0xc, 0x0, 0x0, 0xc, 0xf, 0xcf, 0x2f, 0xf, 0xf,
    0x8, 0xc, 0xc, 0x8, 0x0, 0x0, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xc, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x30, 0x52, 0x61, 0xf1, 0xbc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2, 0x2, 0x3,
    0xd2, 0x63, 0x52, 0x30, 0x0, 0x0, 0x0, 0x0, 0x2, 0x2, 0x4, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x80, 0x48, 0x84, 0xc2, 0xe0, 0x0, 0x0, 0xe0, 0xb4, 0x7c, 0xe1, 0x5b, 0xa5,
    0x68, 0x84, 0xc0, 0x80, 0x0, 0x0, 0x0, 0x0, 0xf5, 0xe1, 0x5a, 0xbe, 0xe0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0xf, 0x33, 0x31, 0x71, 0xf3, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf0, 0xf3, 0x71, 0x31, 0x33, 0xf, 0x0, 0x0, 0x10, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x8, 0xc, 0x8e, 0xcf, 0x88,
    0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x88, 0xcf, 0x8e, 0xc, 0x8, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0xe0, 0xe0, 0xf1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x10, 0x10, 0x20,
    0xe0, 0xf1, 0xe0, 0xe0, 0x0, 0x0, 0x0, 0x0, 0x20, 0x20, 0x10, 0x10, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0xdd,
    0x22, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xff, 0xef, 0x67, 0x77, 0x33, 0x11, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x11, 0x23, 0x67, 0x77, 0xff, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0, 0x79, 0x69, 0xf, 0x1f, 0xff, 0xff, 0x33, 0x0,
    0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0, 0x33, 0x79, 0x69, 0xf, 0x1f, 0xff, 0xff,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xcc, 0xcc, 0x88, 0x0, 0x88, 0xcc, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xcc, 0x88, 0x0, 0x88, 0xcc, 0xcc, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xff, 0xf0, 0xf0, 0xf7, 0x88, 0x0, 0x0, 0x0, 0x0, 0x33, 0x74, 0x74, 0xf8, 0xf9, 0xf9, 0xf9,
    0x0, 0x0, 0x0, 0x88, 0xf7, 0xf0, 0xf0, 0xff, 0xf9, 0xf9, 0xf9, 0xf8, 0x74, 0x74, 0x33, 0x0,
    0xff, 0xf0, 0xf0, 0xff, 0x0, 0x0, 0x0, 0x0, 0xff, 0xf0, 0xf0, 0xff, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0xff, 0xf0, 0xf0, 0xff, 0x0, 0x0, 0x0, 0x0, 0xff, 0xf0, 0xf0, 0xff,
    0x0, 0xcc, 0xe2, 0xe2, 0xf1, 0xf9, 0xf9, 0xf9, 0xff, 0xf0, 0xf0, 0xfe, 0x11, 0x0, 0x0, 0x0,
    0xf9, 0xf9, 0xf9, 0xf1, 0xe2, 0xe2, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x11, 0xfe, 0xf0, 0xf0, 0xff,
    0xff, 0xf0, 0xf0, 0xf0, 0xf0, 0xf8, 0xf8, 0xf8, 0xff, 0xf0, 0xf0, 0xfe, 0x11, 0x0, 0x0, 0x0,
    0xf8, 0xf8, 0xf8, 0xf0, 0xf0, 0xf0, 0xf0, 0xff, 0x0, 0x0, 0x0, 0x11, 0xfe, 0xf0, 0xf0, 0xff,
    0xff, 0xf0, 0xf0, 0xf7, 0x88, 0x0, 0x0, 0x0, 0xff, 0xf0, 0xf0, 0xf0, 0xf0, 0xf1, 0xf1, 0xf1,
    0x0, 0x0, 0x0, 0x88, 0xf7, 0xf0, 0xf0, 0xff, 0xf1, 0xf1, 0xf1, 0xf0, 0xf0, 0xf0, 0xf0, 0xff,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9,
    0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xcc, 0xe2, 0xe2, 0xf1, 0xf1, 0xf1, 0xf1,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf1, 0xf1, 0xf1, 0xf1, 0xe2, 0xe2, 0xcc, 0x0,
    0x0, 0x33, 0x74, 0x74, 0xf8, 0xf8, 0xf8, 0xf8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf8, 0xf8, 0xf8, 0xf8, 0x74, 0x74, 0x33, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf1, 0xf1, 0xf1, 0xf1, 0xf1, 0xf1, 0xf1, 0xf1,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf1, 0xf1, 0xf1, 0xf1, 0xf1, 0xf1, 0xf1, 0xf1,
    0x0, 0x0, 0x0, 0x0, 0x33, 0x74, 0xf8, 0xf8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf8, 0xf8, 0x74, 0x33, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0xff, 0xf0, 0xf0, 0xf0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xf0, 0xf0, 0xf0,
    0xf0, 0xf0, 0xf0, 0xff, 0x0, 0x0, 0x0, 0x0, 0xf0, 0xf0, 0xf0, 0xff, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xcc, 0xe2, 0xf1, 0xf1,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf1, 0xf1, 0xe2, 0xcc, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0xff, 0xf8, 0xf8, 0xf9, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf9, 0xf8, 0xf8, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xf1, 0xf1, 0xf9,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf9, 0xf1, 0xf1, 0xff, 0x0, 0x0, 0x0, 0x0,
    0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xff, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x88, 0xf7, 0xf0, 0xf0, 0xf0, 0xf1, 0xf1, 0xf1, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,
    0xf0, 0xf0, 0xf0, 0xf7, 0x88, 0x0, 0x0, 0x0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf1, 0xf1, 0xf1,
    0xf8, 0xf8, 0xf8, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0x0, 0x0, 0x0, 0x11, 0xfe, 0xf0, 0xf0, 0xf0,
    0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf8, 0xf8, 0xf8, 0xf0, 0xf0, 0xf0, 0xfe, 0x11, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x33, 0x74, 0xf8, 0xf8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf8, 0xf8, 0x74, 0x33, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xcc, 0xe2, 0xf1, 0xf1,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf1, 0xf1, 0xe2, 0xcc, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x88, 0xf7, 0xf0, 0xf0, 0xf0, 0xf9, 0xf9, 0xf9, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8,
    0xf0, 0xf0, 0xf0, 0xf7, 0x88, 0x0, 0x0, 0x0, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf9, 0xf9, 0xf9,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
};

static const uint8_t rom_sprites[4096] = {
    0x0, 0x0, 0x0, 0x8, 0xc, 0xc, 0x8c, 0xc, 0x0, 0x0, 0x30, 0x30, 0x10, 0x10, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0xc0, 0x20, 0x90, 0x80, 0x0, 0x0, 0x0, 0x7, 0xf, 0xf, 0xc3, 0x1f,
    0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x41, 0x21, 0x12, 0x3, 0x3, 0x1, 0x0, 0x0, 0x7, 0x8, 0xf, 0x2f, 0x4f, 0xe, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x8, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x10, 0x10, 0x10,
    0x0, 0x0, 0x0, 0x7, 0x4f, 0xf, 0xa7, 0x87, 0x0, 0x0, 0x0, 0x8, 0xe, 0x8e, 0x1f, 0xf,
    0xc, 0x8, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x33, 0x10, 0x10, 0x10, 0x0, 0x0, 0x0, 0x0,
    0xd3, 0x87, 0x97, 0xf, 0x2f, 0x7, 0x0, 0x0, 0x4f, 0x1f, 0xf, 0x4f, 0xe, 0x8, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x8, 0x8, 0xc, 0xc, 0xc, 0x0, 0x0, 0x0, 0x10, 0x10, 0x30, 0x30, 0x10,
    0x0, 0x0, 0x1, 0x3, 0x87, 0x87, 0x87, 0x47, 0x0, 0x0, 0xe, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xc, 0xc, 0xc, 0x8, 0x8, 0x0, 0x0, 0x0, 0x11, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xef, 0x47, 0x7, 0x7, 0x3, 0x1, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xe, 0x0, 0x0,
    0x0, 0x0, 0xc, 0x68, 0x68, 0x68, 0x6e, 0x6e, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x1, 0x2,
    0x0, 0x0, 0x0, 0x1, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x3, 0xf, 0xf, 0xf, 0xf, 0xf,
    0x68, 0x68, 0x68, 0x68, 0x68, 0xc, 0x0, 0x0, 0x2, 0x1, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf, 0xb, 0xc, 0xf, 0x1, 0x0, 0x0, 0x0, 0xf, 0xf, 0x7, 0xc, 0xf, 0x3, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x8, 0xc, 0xc, 0xc, 0x0, 0x0, 0x0, 0x0, 0x1, 0x1, 0x1, 0x20,
    0x0, 0x0, 0x7, 0xf, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0xc, 0xf, 0xcf, 0x2f, 0xf, 0xf,
    0x8, 0xc, 0xc, 0x8, 0x0, 0x0, 0x0, 0x0, 0x10, 0x0, 0x1, 0x1, 0x1, 0x0, 0x0, 0x0,
    0x87, 0xf, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xc, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x80, 0x48, 0x84, 0xc2, 0xe0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2, 0x2, 0x3,
    0x0, 0x0, 0x0, 0x30, 0x52, 0x61, 0xf1, 0xbc, 0x0, 0x0, 0xe0, 0xb4, 0x7c, 0xe1, 0x5b, 0xa5,
    0x68, 0x84, 0xc0, 0x80, 0x0, 0x0, 0x0, 0x0, 0x2, 0x2, 0x4, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xd2, 0x63, 0x52, 0x30, 0x0, 0x0, 0x0, 0x0, 0xf5, 0xe1, 0x5a, 0xbe, 0xe0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0xf, 0x33, 0x31, 0x71, 0xf3, 0x0, 0x0, 0x0, 0x8, 0xc, 0x8e, 0xcf, 0x88,
    0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x10, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf0, 0xf3, 0x71, 0x31, 0x33, 0xf, 0x0, 0x0, 0xff, 0x88, 0xcf, 0x8e, 0xc, 0x8, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x10, 0x10, 0x20,
    0x0, 0x0, 0x0, 0x0, 0x0, 0xe0, 0xe0, 0xf1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0xdd,
    0x22, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x20, 0x20, 0x10, 0x10, 0x0, 0x0, 0x0, 0x0,
    0xe0, 0xf1, 0xe0, 0xe0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0xee, 0xcc, 0x88, 0x8c, 0x4e, 0xee, 0x88, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x11, 0xff, 0xff, 0x8f, 0xf, 0x3c, 0xbc, 0x0, 0x7f, 0xbf, 0x7f, 0xaf, 0x5f, 0x7f, 0xff,
    0x88, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0xff, 0xff, 0x8f, 0xf, 0x3c, 0xbc, 0x11, 0x0, 0xff, 0xff, 0xff, 0x7f, 0x7f, 0xff, 0xff, 0x0,
    0x0, 0xcc, 0xee, 0xee, 0x8c, 0x8, 0xcc, 0xee, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x11, 0xff, 0xff, 0x8f, 0xf, 0x3c, 0xbc, 0x0, 0x7f, 0xbf, 0x7f, 0xaf, 0x5f, 0x7f, 0xff,
    0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0xcc, 0x0, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0xff, 0xff, 0x8f, 0xf, 0x3c, 0xbc, 0x11, 0x0, 0xff, 0xff, 0xff, 0x7f, 0x7f, 0xff, 0xff, 0x0,
    0x0, 0x0, 0x0, 0xee, 0xee, 0xee, 0xcc, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x60, 0x69, 0xf, 0x71, 0x69, 0xf, 0x17, 0x0, 0x0, 0x0, 0x88, 0xee, 0xff, 0xff, 0xff,
    0xcc, 0xcc, 0xcc, 0xee, 0xee, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x11, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0xff, 0x77, 0x33, 0x0, 0x0, 0x0,
    0x0, 0xee, 0xee, 0xee, 0xcc, 0xcc, 0xcc, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x60, 0x69, 0xf, 0x71, 0x69, 0xf, 0x17, 0x0, 0x0, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xcc, 0xcc, 0xee, 0xee, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x11, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x77, 0x33, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x44, 0xee, 0xee, 0xee, 0xee, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x11, 0x11, 0x0,
    0xee, 0x66, 0x66, 0xee, 0xee, 0xee, 0x66, 0x22, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x44, 0xee, 0xee, 0xee, 0xee, 0x66, 0x66, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x66, 0x66, 0x66, 0x66, 0xee, 0xee, 0x66, 0x22, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x8, 0xc, 0xe, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x7, 0x7, 0x7, 0x3,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x3, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf, 0xf, 0xf, 0x7, 0x3, 0x1, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x7, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3, 0x7, 0x7, 0x7, 0xf, 0xf, 0xf, 0xf,
    0x0, 0x8, 0xc, 0xe, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x0, 0x0, 0x0, 0x8, 0xc, 0xe,
    0xf, 0xf, 0xf, 0xf, 0xe, 0xe, 0xe, 0xc, 0x0, 0x1, 0x3, 0x7, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xc, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xe, 0x0, 0xf, 0xf, 0xf, 0xe, 0xc, 0x8, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xc, 0xe, 0xe, 0xe, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x3, 0x7, 0x0, 0x1, 0x3, 0x7, 0xf, 0xf, 0xf, 0xf,
    0xe, 0xe, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x7, 0x7, 0x7, 0x3,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x3, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf, 0xf, 0xf, 0x7, 0x3, 0x1, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x7, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x3,
    0x0, 0x0, 0x1, 0x3, 0x7, 0xf, 0xf, 0xf, 0x0, 0x0, 0x8, 0x8, 0xc, 0xc, 0xe, 0xe,
    0x0, 0x0, 0x0, 0x8, 0x8, 0x8, 0xc, 0xc, 0x3, 0x7, 0x7, 0x7, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xe, 0xe, 0xe, 0xc, 0x7, 0x7, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xc, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xe, 0x0, 0xf, 0xf, 0xf, 0xe, 0xc, 0x8, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x8, 0xc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x1, 0x1, 0x3, 0x3, 0x7, 0x7, 0x0, 0x0, 0x8, 0xc, 0xe, 0xf, 0xf, 0xf,
    0xc, 0xe, 0xe, 0xe, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x0, 0x1, 0x1, 0x1, 0x3, 0x3,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x7, 0x7, 0x7, 0x3,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x3, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xf, 0xf, 0xf, 0x7, 0x3, 0x1, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x7, 0x0,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x3,
    0x0, 0x0, 0x1, 0x3, 0x7, 0xf, 0xf, 0xf, 0x0, 0x7, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x3, 0x7, 0x7, 0x7, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xe, 0xe, 0xe, 0xc, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xc, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xe, 0x0, 0xf, 0xf, 0xf, 0xe, 0xc, 0x8, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x8, 0xc, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0x0, 0xe, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0x0, 0x0, 0x8, 0xc, 0xe, 0xf, 0xf, 0xf,
    0xc, 0xe, 0xe, 0xe, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf,
    0x0, 0xe0, 0xc0, 0x80, 0xc0, 0xe0, 0xe0, 0x80, 0x0, 0x0, 0x0, 0x10, 0x30, 0x30, 0x70, 0x70,
    0x0, 0x10, 0xf0, 0xf0, 0xf0, 0xf3, 0xf3, 0xf0, 0x0, 0xf0, 0xf1, 0xf2, 0xf2, 0xf1, 0xf1, 0xf2,
    0x80, 0xe0, 0xe0, 0xc0, 0x80, 0xc0, 0xe0, 0x0, 0x70, 0x70, 0x30, 0x30, 0x10, 0x0, 0x0, 0x0,
    0xf0, 0xf3, 0xf3, 0xf0, 0xf0, 0xf0, 0x10, 0x0, 0xf2, 0xf1, 0xf1, 0xf2, 0xf2, 0xf1, 0xf0, 0x0,
    0x0, 0xc0, 0xe0, 0xe0, 0xc0, 0x80, 0xc0, 0xe0, 0x0, 0x0, 0x0, 0x10, 0x30, 0x30, 0x70, 0x70,
    0x0, 0x10, 0xf0, 0xf0, 0xf0, 0xf3, 0xf3, 0xf0, 0x0, 0xf0, 0xf1, 0xf2, 0xf2, 0xf1, 0xf1, 0xf2,
    0xe0, 0xc0, 0x80, 0xc0, 0xe0, 0xe0, 0xc0, 0x0, 0x70, 0x70, 0x30, 0x30, 0x10, 0x0, 0x0, 0x0,
    0xf0, 0xf3, 0xf3, 0xf0, 0xf0, 0xf0, 0x10, 0x0, 0xf2, 0xf1, 0xf1, 0xf2, 0xf2, 0xf1, 0xf0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0x88, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x11, 0xbc, 0x3c, 0xf, 0x8f, 0xff, 0xff, 0x0, 0xff, 0xff, 0x7f, 0x7f, 0xff, 0xff, 0xff,
    0x88, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0xbc, 0x3c, 0xf, 0x8f, 0xff, 0xff, 0x11, 0x0, 0xff, 0x7f, 0x7f, 0xff, 0xff, 0xff, 0xff, 0x0,
    0x0, 0xcc, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x11, 0xbc, 0x3c, 0xf, 0x8f, 0xff, 0xff, 0x0, 0xff, 0xff, 0x7f, 0x7f, 0xff, 0xff, 0xff,
    0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0xcc, 0x0, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0xbc, 0x3c, 0xf, 0x8f, 0xff, 0xff, 0x11, 0x0, 0xff, 0x7f, 0x7f, 0xff, 0xff, 0xff, 0xff, 0x0,
    0x0, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0x88, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x11, 0xff, 0xcf, 0x8f, 0x8f, 0xcf, 0xff, 0x0, 0xff, 0xff, 0x7f, 0xf3, 0xf3, 0x7f, 0xff,
    0x88, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0xff, 0xcf, 0x8f, 0x8f, 0xcf, 0xff, 0x11, 0x0, 0xff, 0x7f, 0xf3, 0xf3, 0x7f, 0xff, 0xff, 0x0,
    0x0, 0xcc, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x11, 0xff, 0xcf, 0x8f, 0x8f, 0xcf, 0xff, 0x0, 0xff, 0xff, 0x7f, 0xf3, 0xf3, 0x7f, 0xff,
    0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0xcc, 0x0, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0xff, 0xcf, 0x8f, 0x8f, 0xcf, 0xff, 0x11, 0x0, 0xff, 0x7f, 0xf3, 0xf3, 0x7f, 0xff, 0xff, 0x0,
    0x0, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0x88, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x11, 0xff, 0xff, 0x8f, 0xf, 0x3c, 0xbc, 0x0, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x7f, 0xff,
    0x88, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0xff, 0xff, 0x8f, 0xf, 0x3c, 0xbc, 0x11, 0x0, 0xff, 0xff, 0xff, 0x7f, 0x7f, 0xff, 0xff, 0x0,
    0x0, 0xcc, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x11, 0xff, 0xff, 0x8f, 0xf, 0x3c, 0xbc, 0x0, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x7f, 0xff,
    0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0xcc, 0x0, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0xff, 0xff, 0x8f, 0xf, 0x3c, 0xbc, 0x11, 0x0, 0xff, 0xff, 0xff, 0x7f, 0x7f, 0xff, 0xff, 0x0,
    0x0, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0x88, 0x0, 0x0, 0x0, 0x1, 0x30, 0x30, 0x67, 0x77,
    0x0, 0x11, 0xff, 0x3f, 0x1f, 0x1f, 0x3f, 0xff, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x88, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x77, 0x67, 0x30, 0x30, 0x1, 0x0, 0x0, 0x0,
    0xff, 0x3f, 0x1f, 0x1f, 0x3f, 0xff, 0x11, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0,
    0x0, 0xcc, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x0, 0x0, 0x1, 0x30, 0x30, 0x67, 0x77,
    0x0, 0x11, 0xff, 0x3f, 0x1f, 0x1f, 0x3f, 0xff, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xee, 0xcc, 0x88, 0xcc, 0xee, 0xee, 0xcc, 0x0, 0x77, 0x67, 0x30, 0x30, 0x1, 0x0, 0x0, 0x0,
    0xff, 0x3f, 0x1f, 0x1f, 0x3f, 0xff, 0x11, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x7, 0x8, 0x8, 0x7, 0x0, 0x7, 0x8, 0x0, 0xc, 0x2, 0x2, 0xc, 0x0, 0xc, 0x2,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x8, 0x7, 0x0, 0x6, 0x9, 0x8, 0x8, 0x6, 0x2, 0xc, 0x0, 0x2, 0x2, 0xa, 0x6, 0x2,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x7, 0x8, 0x8, 0x7, 0x0, 0x7, 0x8, 0x0, 0xc, 0x2, 0x2, 0xc, 0x0, 0xc, 0x2,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x8, 0x7, 0x0, 0x0, 0xf, 0x4, 0x2, 0x1, 0x2, 0xc, 0x0, 0x8, 0xe, 0x8, 0x8, 0x8,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x7, 0x8, 0x8, 0x7, 0x0, 0x7, 0x8, 0x0, 0xc, 0x2, 0x2, 0xc, 0x0, 0xc, 0x2,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x8, 0x7, 0x0, 0x6, 0x9, 0x9, 0x9, 0x6, 0x2, 0xc, 0x0, 0xc, 0x2, 0x2, 0x2, 0xc,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x7, 0x8, 0x8, 0x7, 0x0, 0x7, 0x8, 0x8, 0xc, 0x2, 0x2, 0xc, 0x0, 0xc, 0x2, 0x2,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x7, 0x0, 0x9, 0x9, 0x9, 0x7, 0x0, 0xf, 0xc, 0x0, 0xc, 0x2, 0x2, 0xc, 0x0, 0xe,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xcc, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x66, 0x77,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11,
    0xcc, 0xcc, 0xcc, 0x88, 0x88, 0x0, 0x0, 0x0, 0x77, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0,
    0x88, 0xcc, 0xee, 0xff, 0xff, 0xff, 0x77, 0x0, 0x33, 0x77, 0xff, 0xff, 0xff, 0xff, 0xcc, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x0, 0x77, 0xff, 0xff, 0xff, 0xee, 0xcc, 0x0, 0x0, 0xcc, 0xcc, 0x88, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x77, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0,
    0x88, 0xcc, 0xee, 0xff, 0xff, 0xff, 0x77, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0xcc, 0xcc, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x88, 0x88, 0xcc, 0xcc, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x0, 0x0, 0x88, 0x88, 0xcc, 0xcc, 0xcc, 0x0, 0x0, 0x0, 0x33, 0x33, 0x77, 0x77, 0x77,
    0xcc, 0xcc, 0xcc, 0x88, 0x88, 0x0, 0x0, 0x0, 0x77, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0,
    0xee, 0xee, 0xee, 0xff, 0xff, 0xff, 0x77, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xcc, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x88, 0x88, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x0, 0x77, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0, 0x0, 0xcc, 0xff, 0xff, 0xff, 0xee, 0x0,
    0x0, 0x0, 0x0, 0x88, 0x88, 0x0, 0x0, 0x0, 0x77, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0,
    0xcc, 0xff, 0xff, 0xff, 0xff, 0xff, 0x77, 0x0, 0x0, 0x0, 0xee, 0xff, 0xff, 0xff, 0xcc, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x88, 0x88, 0xcc, 0xcc, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77, 0x77,
    0x0, 0x0, 0x77, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0, 0x0, 0xcc, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xcc, 0xcc, 0xcc, 0x88, 0x88, 0x0, 0x0, 0x0, 0x77, 0x77, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x77, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xcc, 0x0,
    0x1, 0x2, 0x4, 0xc, 0x8, 0x84, 0x84, 0x8, 0x1, 0x0, 0x0, 0x0, 0x9, 0x5, 0x3, 0x0,
    0x0, 0x8, 0x7, 0x78, 0xfa, 0xf5, 0xea, 0x7b, 0x0, 0x0, 0x0, 0xb, 0xb5, 0xea, 0x77, 0x32,
    0x8, 0x84, 0x84, 0x8, 0xc, 0x4, 0x2, 0x1, 0x0, 0x1, 0x1, 0x1, 0x3, 0x4, 0x8, 0x0,
    0x26, 0x5d, 0xb2, 0x7c, 0x7, 0x0, 0x0, 0x0, 0x72, 0xe6, 0xcc, 0xfc, 0xe3, 0xe, 0x0, 0x0,
    0x0, 0x60, 0x60, 0xe0, 0xe0, 0xe8, 0xcc, 0x88, 0x0, 0x0, 0x0, 0x1, 0x0, 0x0, 0x67, 0x77,
    0x0, 0x11, 0xff, 0x3f, 0x1f, 0x1f, 0x3f, 0xff, 0x0, 0x0, 0xf0, 0xf8, 0xfd, 0xff, 0xff, 0xff,
    0x88, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x77, 0x67, 0x0, 0x0, 0x1, 0x0, 0x0, 0x0,
    0xff, 0x3f, 0x1f, 0x1f, 0x3f, 0xff, 0x11, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0,
    0x0, 0x60, 0x60, 0xe0, 0xe0, 0xe8, 0xcc, 0x88, 0x0, 0x0, 0x0, 0x11, 0x23, 0x23, 0x77, 0x77,
    0x0, 0x11, 0xff, 0x1d, 0xc, 0xf, 0x1f, 0xff, 0x0, 0x0, 0xf0, 0xf8, 0xfd, 0xff, 0xff, 0xff,
    0x88, 0xee, 0xee, 0xcc, 0x88, 0xcc, 0xee, 0x0, 0x77, 0x77, 0x23, 0x23, 0x11, 0x0, 0x0, 0x0,
    0xff, 0x1d, 0xc, 0xf, 0x1f, 0xff, 0x11, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x11, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0xff, 0xff, 0xff, 0x77, 0x33, 0x11, 0x0, 0x0, 0x0, 0xcc, 0xee, 0xee, 0xff, 0xff,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x11, 0x0,
    0x0, 0x11, 0x33, 0x77, 0xff, 0xff, 0xff, 0x0, 0xff, 0xff, 0xff, 0xee, 0xee, 0xcc, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x77, 0xff, 0x77, 0x77, 0x33, 0x11, 0x11, 0x0, 0x0, 0xcc, 0xee, 0xee, 0xff, 0xff, 0xff,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x11, 0x11, 0x33, 0x77, 0x77, 0xff, 0x77, 0xee, 0xff, 0xff, 0xff, 0xee, 0xee, 0xcc, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x33, 0x33, 0x33, 0x11, 0x11, 0x11, 0x0, 0x0, 0x88, 0xcc, 0xee, 0xee, 0xff, 0xff, 0xff,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x11, 0x11, 0x11, 0x33, 0x33, 0x33, 0xee, 0xff, 0xff, 0xff, 0xee, 0xee, 0xcc, 0x88,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x11, 0x11, 0x11, 0x11, 0x0, 0x0, 0x0, 0x0, 0xcc, 0xee, 0xee, 0xff, 0xff, 0xff, 0xff,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x11, 0x11, 0x11, 0x11, 0xee, 0xff, 0xff, 0xff, 0xff, 0xee, 0xee, 0xcc,
    0x0, 0x0, 0x0, 0x0, 0x88, 0x88, 0x88, 0x88, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x66, 0x77, 0x77, 0x77, 0xff, 0xff, 0xff,
    0x0, 0x88, 0x88, 0x88, 0x88, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0xff, 0xff, 0x77, 0x77, 0x77, 0x66,
    0x0, 0x0, 0x88, 0x88, 0xcc, 0xcc, 0xcc, 0x88, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x33, 0x77, 0x77, 0xff,
    0x0, 0x88, 0xcc, 0xcc, 0xcc, 0x88, 0x88, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0x77, 0x77, 0x33, 0x33, 0x33, 0x11,
    0x0, 0x0, 0xcc, 0xee, 0xee, 0xee, 0xee, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x11, 0x33, 0x33, 0x77,
    0x88, 0xcc, 0xee, 0xee, 0xee, 0xee, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x77, 0x33, 0x33, 0x11, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x44, 0xee, 0xee, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x33, 0x77,
    0xcc, 0xee, 0xee, 0xee, 0x44, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x77, 0x33, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x44, 0xee, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x33,
    0xcc, 0xee, 0x44, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x33, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xcc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x22, 0x44, 0x11, 0x22, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x22, 0x11, 0x0, 0x0, 0x0, 0x11, 0x99, 0x44, 0x0, 0x0,
    0x0, 0x22, 0x11, 0x88, 0x44, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x11, 0x22, 0x0, 0x11, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x88, 0x22, 0x22, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
};


static const uint8_t rom_hwcolors[32] = {
    0x0, 0x7, 0x66, 0xef, 0x0, 0xf8, 0xea, 0x6f, 0x0, 0x3f, 0x0, 0xc9, 0x38, 0xaa, 0xaf, 0xf6,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
};

static const uint8_t rom_palette[256] = {
    0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0xb, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0xb, 0x3,
    0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0xb, 0x5, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0xb, 0x7,
    0x0, 0x0, 0x0, 0x0, 0x0, 0xb, 0x1, 0x9, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0x0, 0xe, 0x0, 0x1, 0xc, 0xf,
    0x0, 0xe, 0x0, 0xb, 0x0, 0xc, 0xb, 0xe, 0x0, 0xc, 0xf, 0x1, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x1, 0x2, 0xf, 0x0, 0x7, 0xc, 0x2, 0x0, 0x9, 0x6, 0xf, 0x0, 0xd, 0xc, 0xf,
    0x0, 0x5, 0x3, 0x9, 0x0, 0xf, 0xb, 0x0, 0x0, 0xe, 0x0, 0xb, 0x0, 0xe, 0x0, 0xb,
    0x0, 0x0, 0x0, 0x0, 0x0, 0xf, 0xe, 0x1, 0x0, 0xf, 0xb, 0xe, 0x0, 0xe, 0x0, 0xf,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
};

static const uint8_t rom_wavetable[256] = {
    0x7, 0x9, 0xa, 0xb, 0xc, 0xd, 0xd, 0xe, 0xe, 0xe, 0xd, 0xd, 0xc, 0xb, 0xa, 0x9,
    0x7, 0x5, 0x4, 0x3, 0x2, 0x1, 0x1, 0x0, 0x0, 0x0, 0x1, 0x1, 0x2, 0x3, 0x4, 0x5,
    0x7, 0xc, 0xe, 0xe, 0xd, 0xb, 0x9, 0xa, 0xb, 0xb, 0xa, 0x9, 0x6, 0x4, 0x3, 0x5,
    0x7, 0x9, 0xb, 0xa, 0x8, 0x5, 0x4, 0x3, 0x3, 0x4, 0x5, 0x3, 0x1, 0x0, 0x0, 0x2,
    0x7, 0xa, 0xc, 0xd, 0xe, 0xd, 0xc, 0xa, 0x7, 0x4, 0x2, 0x1, 0x0, 0x1, 0x2, 0x4,
    0x7, 0xb, 0xd, 0xe, 0xd, 0xb, 0x7, 0x3, 0x1, 0x0, 0x1, 0x3, 0x7, 0xe, 0x7, 0x0,
    0x7, 0xd, 0xb, 0x8, 0xb, 0xd, 0x9, 0x6, 0xb, 0xe, 0xc, 0x7, 0x9, 0xa, 0x6, 0x2,
    0x7, 0xc, 0x8, 0x4, 0x5, 0x7, 0x2, 0x0, 0x3, 0x8, 0x5, 0x1, 0x3, 0x6, 0x3, 0x1,
    0x0, 0x8, 0xf, 0x7, 0x1, 0x8, 0xe, 0x7, 0x2, 0x8, 0xd, 0x7, 0x3, 0x8, 0xc, 0x7,
    0x4, 0x8, 0xb, 0x7, 0x5, 0x8, 0xa, 0x7, 0x6, 0x8, 0x9, 0x7, 0x7, 0x8, 0x8, 0x7,
    0x7, 0x8, 0x6, 0x9, 0x5, 0xa, 0x4, 0xb, 0x3, 0xc, 0x2, 0xd, 0x1, 0xe, 0x0, 0xf,
    0x0, 0xf, 0x1, 0xe, 0x2, 0xd, 0x3, 0xc, 0x4, 0xb, 0x5, 0xa, 0x6, 0x9, 0x7, 0x8,
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
    0xf, 0xe, 0xd, 0xc, 0xb, 0xa, 0x9, 0x8, 0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0x0,
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
};

/*== SOUND EFFECT REGISTER DUMPS =============================================*/

/*
    Each line is a 'register dump' for one 60Hz tick. Each 32-bit number
    encodes the per-voice values for frequency, waveform and volume:

    31                              0 bit
    |vvvv-www----ffffffffffffffffffff|
      |    |              |
      |    |              +-- 20 bits frequency
      |    +-- 3 bits waveform
      +-- 4 bits volume
*/
static const uint32_t snd_dump_prelude[490] = {
    0xE20002E0, 0xF0001700,
    0xD20002E0, 0xF0001700,
    0xC20002E0, 0xF0001700,
    0xB20002E0, 0xF0001700,
    0xA20002E0, 0xF0000000,
    0x920002E0, 0xF0000000,
    0x820002E0, 0xF0000000,
    0x720002E0, 0xF0000000,
    0x620002E0, 0xF0002E00,
    0x520002E0, 0xF0002E00,
    0x420002E0, 0xF0002E00,
    0x320002E0, 0xF0002E00,
    0x220002E0, 0xF0000000,
    0x120002E0, 0xF0000000,
    0x020002E0, 0xF0000000,
    0xE2000000, 0xF0002280,
    0xD2000000, 0xF0002280,
    0xC2000000, 0xF0002280,
    0xB2000000, 0xF0002280,
    0xA2000000, 0xF0000000,
    0x92000000, 0xF0000000,
    0x82000000, 0xF0000000,
    0x72000000, 0xF0000000,
    0xE2000450, 0xF0001D00,
    0xD2000450, 0xF0001D00,
    0xC2000450, 0xF0001D00,
    0xB2000450, 0xF0001D00,
    0xA2000450, 0xF0000000,
    0x92000450, 0xF0000000,
    0x82000450, 0xF0000000,
    0x72000450, 0xF0000000,
    0xE20002E0, 0xF0002E00,
    0xD20002E0, 0xF0002E00,
    0xC20002E0, 0xF0002E00,
    0xB20002E0, 0xF0002E00,
    0xA20002E0, 0xF0002280,
    0x920002E0, 0xF0002280,
    0x820002E0, 0xF0002280,
    0x720002E0, 0xF0002280,
    0x620002E0, 0xF0000000,
    0x520002E0, 0xF0000000,
    0x420002E0, 0xF0000000,
    0x320002E0, 0xF0000000,
    0x220002E0, 0xF0000000,
    0x120002E0, 0xF0000000,
    0x020002E0, 0xF0000000,
    0xE2000000, 0xF0001D00,
    0xD2000000, 0xF0001D00,
    0xC2000000, 0xF0001D00,
    0xB2000000, 0xF0001D00,
    0xA2000000, 0xF0001D00,
    0x92000000, 0xF0001D00,
    0x82000000, 0xF0001D00,
    0x72000000, 0xF0001D00,
    0xE2000450, 0xF0000000,
    0xD2000450, 0xF0000000,
    0xC2000450, 0xF0000000,
    0xB2000450, 0xF0000000,
    0xA2000450, 0xF0000000,
    0x92000450, 0xF0000000,
    0x82000450, 0xF0000000,
    0x72000450, 0xF0000000,
    0xE2000308, 0xF0001840,
    0xD2000308, 0xF0001840,
    0xC2000308, 0xF0001840,
    0xB2000308, 0xF0001840,
    0xA2000308, 0xF0000000,
    0x92000308, 0xF0000000,
    0x82000308, 0xF0000000,
    0x72000308, 0xF0000000,
    0x62000308, 0xF00030C0,
    0x52000308, 0xF00030C0,
    0x42000308, 0xF00030C0,
    0x32000308, 0xF00030C0,
    0x22000308, 0xF0000000,
    0x12000308, 0xF0000000,
    0x02000308, 0xF0000000,
    0xE2000000, 0xF0002480,
    0xD2000000, 0xF0002480,
    0xC2000000, 0xF0002480,
    0xB2000000, 0xF0002480,
    0xA2000000, 0xF0000000,
    0x92000000, 0xF0000000,
    0x82000000, 0xF0000000,
    0x72000000, 0xF0000000,
    0xE2000490, 0xF0001EC0,
    0xD2000490, 0xF0001EC0,
    0xC2000490, 0xF0001EC0,
    0xB2000490, 0xF0001EC0,
    0xA2000490, 0xF0000000,
    0x92000490, 0xF0000000,
    0x82000490, 0xF0000000,
    0x72000490, 0xF0000000,
    0xE2000308, 0xF00030C0,
    0xD2000308, 0xF00030C0,
    0xC2000308, 0xF00030C0,
    0xB2000308, 0xF00030C0,
    0xA2000308, 0xF0002480,
    0x92000308, 0xF0002480,
    0x82000308, 0xF0002480,
    0x72000308, 0xF0002480,
    0x62000308, 0xF0000000,
    0x52000308, 0xF0000000,
    0x42000308, 0xF0000000,
    0x32000308, 0xF0000000,
    0x22000308, 0xF0000000,
    0x12000308, 0xF0000000,
    0x02000308, 0xF0000000,
    0xE2000000, 0xF0001EC0,
    0xD2000000, 0xF0001EC0,
    0xC2000000, 0xF0001EC0,
    0xB2000000, 0xF0001EC0,
    0xA2000000, 0xF0001EC0,
    0x92000000, 0xF0001EC0,
    0x82000000, 0xF0001EC0,
    0x72000000, 0xF0001EC0,
    0xE2000490, 0xF0000000,
    0xD2000490, 0xF0000000,
    0xC2000490, 0xF0000000,
    0xB2000490, 0xF0000000,
    0xA2000490, 0xF0000000,
    0x92000490, 0xF0000000,
    0x82000490, 0xF0000000,
    0x72000490, 0xF0000000,
    0xE20002E0, 0xF0001700,
    0xD20002E0, 0xF0001700,
    0xC20002E0, 0xF0001700,
    0xB20002E0, 0xF0001700,
    0xA20002E0, 0xF0000000,
    0x920002E0, 0xF0000000,
    0x820002E0, 0xF0000000,
    0x720002E0, 0xF0000000,
    0x620002E0, 0xF0002E00,
    0x520002E0, 0xF0002E00,
    0x420002E0, 0xF0002E00,
    0x320002E0, 0xF0002E00,
    0x220002E0, 0xF0000000,
    0x120002E0, 0xF0000000,
    0x020002E0, 0xF0000000,
    0xE2000000, 0xF0002280,
    0xD2000000, 0xF0002280,
    0xC2000000, 0xF0002280,
    0xB2000000, 0xF0002280,
    0xA2000000, 0xF0000000,
    0x92000000, 0xF0000000,
    0x82000000, 0xF0000000,
    0x72000000, 0xF0000000,
    0xE2000450, 0xF0001D00,
    0xD2000450, 0xF0001D00,
    0xC2000450, 0xF0001D00,
    0xB2000450, 0xF0001D00,
    0xA2000450, 0xF0000000,
    0x92000450, 0xF0000000,
    0x82000450, 0xF0000000,
    0x72000450, 0xF0000000,
    0xE20002E0, 0xF0002E00,
    0xD20002E0, 0xF0002E00,
    0xC20002E0, 0xF0002E00,
    0xB20002E0, 0xF0002E00,
    0xA20002E0, 0xF0002280,
    0x920002E0, 0xF0002280,
    0x820002E0, 0xF0002280,
    0x720002E0, 0xF0002280,
    0x620002E0, 0xF0000000,
    0x520002E0, 0xF0000000,
    0x420002E0, 0xF0000000,
    0x320002E0, 0xF0000000,
    0x220002E0, 0xF0000000,
    0x120002E0, 0xF0000000,
    0x020002E0, 0xF0000000,
    0xE2000000, 0xF0001D00,
    0xD2000000, 0xF0001D00,
    0xC2000000, 0xF0001D00,
    0xB2000000, 0xF0001D00,
    0xA2000000, 0xF0001D00,
    0x92000000, 0xF0001D00,
    0x82000000, 0xF0001D00,
    0x72000000, 0xF0001D00,
    0xE2000450, 0xF0000000,
    0xD2000450, 0xF0000000,
    0xC2000450, 0xF0000000,
    0xB2000450, 0xF0000000,
    0xA2000450, 0xF0000000,
    0x92000450, 0xF0000000,
    0x82000450, 0xF0000000,
    0x72000450, 0xF0000000,
    0xE2000450, 0xF0001B40,
    0xD2000450, 0xF0001B40,
    0xC2000450, 0xF0001B40,
    0xB2000450, 0xF0001B40,
    0xA2000450, 0xF0001D00,
    0x92000450, 0xF0001D00,
    0x82000450, 0xF0001D00,
    0x72000450, 0xF0001D00,
    0x62000450, 0xF0001EC0,
    0x52000450, 0xF0001EC0,
    0x42000450, 0xF0001EC0,
    0x32000450, 0xF0001EC0,
    0x22000450, 0xF0000000,
    0x12000450, 0xF0000000,
    0x02000450, 0xF0000000,
    0xE20004D0, 0xF0001EC0,
    0xD20004D0, 0xF0001EC0,
    0xC20004D0, 0xF0001EC0,
    0xB20004D0, 0xF0001EC0,
    0xA20004D0, 0xF0002080,
    0x920004D0, 0xF0002080,
    0x820004D0, 0xF0002080,
    0x720004D0, 0xF0002080,
    0x620004D0, 0xF0002280,
    0x520004D0, 0xF0002280,
    0x420004D0, 0xF0002280,
    0x320004D0, 0xF0002280,
    0x220004D0, 0xF0000000,
    0x120004D0, 0xF0000000,
    0x020004D0, 0xF0000000,
    0xE2000568, 0xF0002280,
    0xD2000568, 0xF0002280,
    0xC2000568, 0xF0002280,
    0xB2000568, 0xF0002280,
    0xA2000568, 0xF0002480,
    0x92000568, 0xF0002480,
    0x82000568, 0xF0002480,
    0x72000568, 0xF0002480,
    0x62000568, 0xF0002680,
    0x52000568, 0xF0002680,
    0x42000568, 0xF0002680,
    0x32000568, 0xF0002680,
    0x22000568, 0xF0000000,
    0x12000568, 0xF0000000,
    0x02000568, 0xF0000000,
    0xE20005C0, 0xF0002E00,
    0xD20005C0, 0xF0002E00,
    0xC20005C0, 0xF0002E00,
    0xB20005C0, 0xF0002E00,
    0xA20005C0, 0xF0002E00,
    0x920005C0, 0xF0002E00,
    0x820005C0, 0xF0002E00,
    0x720005C0, 0xF0002E00,
    0x620005C0, 0x00000E80,
    0x520005C0, 0x00000E80,
    0x420005C0, 0x00000E80,
    0x320005C0, 0x00000E80,
    0x220005C0, 0x00000E80,
    0x120005C0, 0x00000E80,
};

static const uint32_t snd_dump_dead[90] = {
    0xF1001F00,
    0xF1001E00,
    0xF1001D00,
    0xF1001C00,
    0xF1001B00,
    0xF1001C00,
    0xF1001D00,
    0xF1001E00,
    0xF1001F00,
    0xF1002000,
    0xF1002100,
    0xE1001D00,
    0xE1001C00,
    0xE1001B00,
    0xE1001A00,
    0xE1001900,
    0xE1001800,
    0xE1001900,
    0xE1001A00,
    0xE1001B00,
    0xE1001C00,
    0xE1001D00,
    0xE1001E00,
    0xD1001B00,
    0xD1001A00,
    0xD1001900,
    0xD1001800,
    0xD1001700,
    0xD1001600,
    0xD1001700,
    0xD1001800,
    0xD1001900,
    0xD1001A00,
    0xD1001B00,
    0xD1001C00,
    0xC1001900,
    0xC1001800,
    0xC1001700,
    0xC1001600,
    0xC1001500,
    0xC1001400,
    0xC1001500,
    0xC1001600,
    0xC1001700,
    0xC1001800,
    0xC1001900,
    0xC1001A00,
    0xB1001700,
    0xB1001600,
    0xB1001500,
    0xB1001400,
    0xB1001300,
    0xB1001200,
    0xB1001300,
    0xB1001400,
    0xB1001500,
    0xB1001600,
    0xB1001700,
    0xB1001800,
    0xA1001500,
    0xA1001400,
    0xA1001300,
    0xA1001200,
    0xA1001100,
    0xA1001000,
    0xA1001100,
    0xA1001200,
    0x80000800,
    0x80001000,
    0x80001800,
    0x80002000,
    0x80002800,
    0x80003000,
    0x80003800,
    0x80004000,
    0x80004800,
    0x80005000,
    0x80005800,
    0x00000000,
    0x80000800,
    0x80001000,
    0x80001800,
    0x80002000,
    0x80002800,
    0x80003000,
    0x80003800,
    0x80004000,
    0x80004800,
    0x80005000,
    0x80005800,
};
