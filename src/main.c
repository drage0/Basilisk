/*
 * Basilisk.
 * Software for aiding creation of 2D plans.
 * The exported file format is explain right before the defenition of BAS_ExportWorld.
 *
 * Timestamp - 02.09.2019.
 */
#include <stdio.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

/* Simple preprocessor defines for boilerplate code */
#define BAS_UseColour(r, g, b) SDL_SetRenderDrawColor(renderer, r, g, b, 0xff)
#define BAS_UseColourAlpha(r, g, b, a) SDL_SetRenderDrawColor(renderer, r, g, b, a)
#define BAS_Present SDL_RenderPresent(renderer)
#define BAS_Clear SDL_RenderClear(renderer)

/*
 * Define the editor
 */
#define BASILISK_VERSION "0.0.1"
#define BAS_NO_SUCH_VERTEX -1
#define BAS_NO_SUCH_LINE -1
#define BAS_NO_SUCH_ROOM -1
#define BAS_NO_SUCH_THING -1
#define CURSOR_ARROW 0
#define CURSOR_CROSSHAIR 1
#define CURSOR_HAND 2
#define CURSOR_CROSSBONES 3
#define CELL_SCALE 32
#define THING_SCALE (CELL_SCALE/4)
static const int NORMAL_LENGTH         = 4;
static const int NORMAL_COLOUR[3]      = {200, 0,   150};
static int CROSSHAIR_COLOUR[3]         = {200, 200, 200};
static const SDL_Color TEXT_BACKGROUND = {0, 0, 0, 255};
static const SDL_Color TEXT_COLOUR     = {255, 255, 255, 255};
static char statusline[2][128];
static const char* const FONT_PATH     = "data/default.ttf";
static SDL_Cursor* cursorheap[4];
static TTF_Font* font_default;
static TTF_Font* font_textinput;
static SDL_Texture *basilisk_texture = NULL;

/*
 * Editor data
 */
#define MAX_ROOM_COUNT  1024
#define MAX_LINE_COUNT  1024
#define MAX_THING_COUNT 1024
struct BAS_Room
{
	int cellposition[2]; /* (x, y) position of the room (cell-space). */
};
struct BAS_Line
{
	int cellnodeposition[2][2];
	int normal[2][2]; /* Two vertices representing normal vector */
};
struct BAS_Thing
{
	uint64_t flags;
	int thingposition[2]; /* (x, y) position of the thing (thing-space). */
	int type;
	int facing;
};
static struct BAS_Room  rooms[MAX_ROOM_COUNT];
static struct BAS_Line  lines[MAX_LINE_COUNT];
static struct BAS_Thing things[MAX_THING_COUNT];
static int room_count  = 0;
static int line_count  = 0;
static int thing_count = 0;

/*
 * Required for windowing system
 */
static SDL_Window* window;
static SDL_Renderer* renderer;
static const int WINDOW_WIDTH  = CELL_SCALE*32+1;
static const int WINDOW_HEIGHT = CELL_SCALE*22+1;

/*
 * Event system additions
 */
static const int DELAY_PER_FRAME_DEFAULT = 30;
static const int DELAY_PER_FRAME_ABSENT  = 70;
static const int DELAY_PER_FRAME_MOTION  = 15;
static Uint32 delayperframe;

/*
 * Printing of messages
 */
#define WRITE_I(s) BAS_PrintInfo(s)
#define WRITE_W(s) BAS_PrintWarning(s)
#define WRITE_E(s) BAS_PrintError(s)
static inline void
BAS_PrintInfo(const char* s)
{
	printf("[i] %s\n", s);
}
static inline void
BAS_PrintWarning(const char* s)
{
	printf("[W] %s\n", s);
}
static inline void
BAS_PrintError(const char* s)
{
	printf("[X] %s\n", s);
}

/*
 * Each tool has its own way of execution.
 * Changing tool changes the function called each frame.
 */
#define TOOL_SPECIAL_VOID       0 /* Nothing. */
#define TOOL_SPECIAL_RESETSTATE 1 /* Same tool ran again. */
#define TOOL_SPECIAL_STOP       2 /* Tool changed, ran on the previous tool. */
#define TOOL_SPECIAL_BEGIN      3 /* Tool changed, ran on the new tool. */
typedef void (*executionjump)(SDL_Event, int, int, int);
typedef void (*drawexectionjump)(int, int);

/* Snap the given coordinate to the closest cell's node or the closest cell. */
inline static void
BAS_SnapToClosestNode(int* x, int* y)
{
	*x = (int)((float)(*x)/CELL_SCALE+0.5)*CELL_SCALE;
	*y = (int)((float)(*y)/CELL_SCALE+0.5)*CELL_SCALE;
}
inline static void
BAS_SnapToClosestNode_Custom(int* x, int* y, const int scale)
{
	*x = (int)((float)(*x)/scale+0.5)*scale;
	*y = (int)((float)(*y)/scale+0.5)*scale;
}
inline static void
BAS_SnapToClosestCell(int* x, int* y)
{
	*x -= CELL_SCALE/2;
	*y -= CELL_SCALE/2;
	BAS_SnapToClosestNode(x, y);
}
inline static void
BAS_SnapToClosestCell_Custom(int* x, int* y, const int scale)
{
	*x -= scale/2;
	*y -= scale/2;
	BAS_SnapToClosestNode_Custom(x, y, scale);
}

/* Calculate the closest cell's position to the given coordinates. */
static void
BAS_ClosestCellPosition(int fx, int fy, int *cx, int *cy)
{
	*cx = fx;
	*cy = fy;
	BAS_SnapToClosestCell(cx, cy);
	*cx /= CELL_SCALE;
	*cy /= CELL_SCALE;
}

/*
 * Given two vertices, calculate the normal of the line the two vertices create.
 */
static void
BAS_CalculateLineNormalVertices(struct BAS_Line *line)
{
	int x0 = line->cellnodeposition[0][0]*CELL_SCALE;
	int y0 = line->cellnodeposition[0][1]*CELL_SCALE;
	int x1 = line->cellnodeposition[1][0]*CELL_SCALE;
	int y1 = line->cellnodeposition[1][1]*CELL_SCALE;
	int length;
	length = (int) sqrt(pow(x0-x1, 2)+pow(y0-y1, 2));
	line->normal[0][0] = (x0+x1)/2;
	line->normal[0][1] = (y0+y1)/2;
	line->normal[1][0] = NORMAL_LENGTH*(x0-x1)/length;
	line->normal[1][1] = NORMAL_LENGTH*(y0-y1)/length;
}

/*
 * Create a texture from the given string.
 * The texture should be freed one it's of no use.
 */
static SDL_Texture*
BAS_CreateTextTexture(TTF_Font* font, const char* text)
{
	SDL_Texture* texture;
	SDL_Surface* temporarysuface;
	temporarysuface = TTF_RenderUTF8_Shaded(font, text, TEXT_COLOUR, TEXT_BACKGROUND);
	texture         = SDL_CreateTextureFromSurface(renderer, temporarysuface);
	SDL_FreeSurface(temporarysuface);
	return texture;
}
static SDL_Texture*
BAS_CreateTextTextureBlended(TTF_Font* font, const char* text)
{
	SDL_Texture* texture;
	SDL_Surface* temporarysuface;
	temporarysuface = TTF_RenderUTF8_Blended(font, text, TEXT_COLOUR);
	texture         = SDL_CreateTextureFromSurface(renderer, temporarysuface);
	SDL_FreeSurface(temporarysuface);
	return texture;
}

/*
 * If the given cell coordinates do not correspond to a room, create a new room (returns 0).
 * Otherwise, returns 1.
 */
static int
BAS_Room_Create(int cx, int cy)
{
	register int i;
	for (i = 0; i < room_count; i++)
	{
		if (rooms[i].cellposition[0] == cx && rooms[i].cellposition[1] == cy)
		{
			return 1;
		}
	}
	rooms[room_count].cellposition[0] = cx;
	rooms[room_count].cellposition[1] = cy;
	room_count++;
	return 0;
}

static int
BAS_FindRoom(int cx, int cy)
{
	register int i;
	for (i = 0; i < room_count; i++)
	{
		if (rooms[i].cellposition[0] == cx && rooms[i].cellposition[1] == cy)
		{
			return i;
		}
	}
	return BAS_NO_SUCH_ROOM;
}

static int
BAS_FindThing(int cx, int cy)
{
	register int i;
	for (i = 0; i < thing_count; i++)
	{
		if (things[i].thingposition[0] == cx && things[i].thingposition[1] == cy)
		{
			return i;
		}
	}
	return BAS_NO_SUCH_THING;
}

static inline void
BAS_Line_Create(int x0, int y0, int x1, int y1)
{
	lines[line_count].cellnodeposition[0][0] = x0;
	lines[line_count].cellnodeposition[0][1] = y0;
	lines[line_count].cellnodeposition[1][0] = x1;
	lines[line_count].cellnodeposition[1][1] = y1;
	BAS_CalculateLineNormalVertices(&lines[line_count]);
	line_count++;
}

static int walkedrooms[MAX_ROOM_COUNT];
static void
walkrooms(int room_index)
{
	const int room_cx = rooms[room_index].cellposition[0];
	const int room_cy = rooms[room_index].cellposition[1];
	int neighbour_north, neighbour_south, neighbour_west, neighbour_east;

	if (walkedrooms[room_index])
	{
		return;
	}
	walkedrooms[room_index] = 1;

	/* Test north/south/west/east side */
	neighbour_north = BAS_FindRoom(room_cx, room_cy-1);
	neighbour_south = BAS_FindRoom(room_cx, room_cy+1);
	neighbour_west  = BAS_FindRoom(room_cx-1, room_cy);
	neighbour_east  = BAS_FindRoom(room_cx+1, room_cy);

	if (neighbour_north == BAS_NO_SUCH_ROOM)
	{
		BAS_Line_Create(room_cx+1, room_cy, room_cx, room_cy);
	}
	else
	{
		walkrooms(neighbour_north);
	}
	if (neighbour_south == BAS_NO_SUCH_ROOM)
	{
		BAS_Line_Create(room_cx, room_cy+1, room_cx+1, room_cy+1);
	}
	else
	{
		walkrooms(neighbour_south);
	}
	if (neighbour_west == BAS_NO_SUCH_ROOM)
	{
		BAS_Line_Create(room_cx, room_cy, room_cx, room_cy+1);
	}
	else
	{
		walkrooms(neighbour_west);
	}
	if (neighbour_east == BAS_NO_SUCH_ROOM)
	{
		BAS_Line_Create(room_cx+1, room_cy+1, room_cx+1, room_cy);
	}
	else
	{
		walkrooms(neighbour_east);
	}
}

static void
BAS_RecalculateLines(void)
{
	if (room_count < 0)
	{
		WRITE_W("room_count < 0, not calculating lines.");
		return;
	}
	/*
	 * Set line count and all walked rooms to zero. Afterwards, recursively walk
	 * through all the rooms.
	 */
	line_count = 0;
	memset(walkedrooms, 0, sizeof(walkedrooms));
	walkrooms(0);
}

/*
 * Use a new message for the status line.
 */
#define BAS_STATUSMESSAGE_LENGTH 128
#define BAS_STATUSMESSAGE_TYPE_INFO 0
#define BAS_STATUSMESSAGE_TYPE_WARNING 1
#define BAS_STATUSMESSAGE_TYPE_ERROR 2
static const SDL_Color TEXTCOLOUR_INFO    = {255, 255, 255, 255};
static const SDL_Color TEXTCOLOUR_WARNING = {255, 255, 0, 255};
static const SDL_Color TEXTCOLOUR_ERROR   = {255, 128, 0, 255};
static SDL_Surface *statuslinesurface     = NULL;
static SDL_Texture *statuslinetexture[2]  = {NULL, NULL};
static void
BAS_PushStatus(int type, const char status0[BAS_STATUSMESSAGE_LENGTH], const char status1[BAS_STATUSMESSAGE_LENGTH])
{
	SDL_Color textcolour = TEXTCOLOUR_INFO;
	switch(type)
	{
		case BAS_STATUSMESSAGE_TYPE_WARNING: textcolour = TEXTCOLOUR_WARNING; break;
		case BAS_STATUSMESSAGE_TYPE_ERROR:   textcolour = TEXTCOLOUR_ERROR;   break;
	}
	strncpy(statusline[0], status0, (BAS_STATUSMESSAGE_LENGTH-1)*sizeof(char));
	strncpy(statusline[1], status1, (BAS_STATUSMESSAGE_LENGTH-1)*sizeof(char));
	SDL_FreeSurface(statuslinesurface);
	SDL_DestroyTexture(statuslinetexture[0]);
	SDL_DestroyTexture(statuslinetexture[1]);
	statuslinesurface    = TTF_RenderText_Shaded(font_default, statusline[0], textcolour, TEXT_BACKGROUND);
	statuslinetexture[0] = SDL_CreateTextureFromSurface(renderer, statuslinesurface);
	SDL_FreeSurface(statuslinesurface);
	statuslinesurface    = TTF_RenderText_Shaded(font_default, statusline[1], textcolour, TEXT_BACKGROUND);
	statuslinetexture[1] = SDL_CreateTextureFromSurface(renderer, statuslinesurface);
}
static inline void
BAS_PushStatusAndWriteInfo(const char *message)
{
	WRITE_I(message);
	BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_INFO, "[info]", message);
}
static inline void
BAS_PushStatusAndWriteWarning(const char *message)
{
	WRITE_W(message);
	BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_WARNING, "[warning]", message);
}
static inline void
BAS_PushStatusAndWriteError(const char *message)
{
	WRITE_E(message);
	BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_ERROR, "[error]", message);
}

/* What says on the tin. */
static void
BAS_DrawGrid(void)
{
	register int i;
	BAS_UseColour(32, 32, 32);
	for (i = 0; i < WINDOW_WIDTH; i += CELL_SCALE)
	{
		SDL_RenderDrawLine(renderer, i, 0, i, WINDOW_HEIGHT);
	}
	for (i = 0; i < WINDOW_HEIGHT; i += CELL_SCALE)
	{
		SDL_RenderDrawLine(renderer, 0, i, WINDOW_WIDTH, i);
	}
}

static void
BAS_DrawCrosshair(void)
{
	int mx, my;
	BAS_UseColour(CROSSHAIR_COLOUR[0], CROSSHAIR_COLOUR[1], CROSSHAIR_COLOUR[2]);
	SDL_GetMouseState(&mx, &my);
	SDL_RenderDrawLine(renderer, mx, 0, mx, WINDOW_HEIGHT);
	SDL_RenderDrawLine(renderer, 0, my, WINDOW_WIDTH, my);
	BAS_UseColour(255, 255, 0);
}

static void
BAS_DrawCrosshair_Small(int mx, int my)
{
	SDL_Rect rectangle;
	const int length = 96;
	BAS_UseColour(CROSSHAIR_COLOUR[0], CROSSHAIR_COLOUR[1], CROSSHAIR_COLOUR[2]);
	rectangle.x = mx-THING_SCALE/2;
	rectangle.y = my-THING_SCALE/2;
	rectangle.w = rectangle.h = THING_SCALE;
	SDL_RenderDrawLine(renderer, mx, my-length, mx, my-THING_SCALE/2);
	SDL_RenderDrawLine(renderer, mx, my+THING_SCALE/2, mx, my+length);
	SDL_RenderDrawLine(renderer, mx-length, my, mx-THING_SCALE/2, my);
	SDL_RenderDrawLine(renderer, mx+THING_SCALE/2, my, mx+length, my);
	SDL_RenderDrawRect(renderer, &rectangle);
}

static void
BAS_DrawRooms(void)
{
	register int i;
	BAS_UseColourAlpha(0, 255, 0, 60);
	for (i = 0; i < room_count; i++)
	{
		const int plan_x = rooms[i].cellposition[0]*CELL_SCALE;
		const int plan_y = rooms[i].cellposition[1]*CELL_SCALE;
		SDL_Rect rectangle;
		rectangle.x = plan_x;
		rectangle.y = plan_y;
		rectangle.w = CELL_SCALE;
		rectangle.h = CELL_SCALE;
		SDL_RenderFillRect(renderer, &rectangle);
	}
}

static void
BAS_DrawLines(void)
{
	register int i;
	for (i = 0; i < line_count; i++)
	{
		const int x0 = lines[i].cellnodeposition[0][0]*CELL_SCALE;
		const int y0 = lines[i].cellnodeposition[0][1]*CELL_SCALE;
		const int x1 = lines[i].cellnodeposition[1][0]*CELL_SCALE;
		const int y1 = lines[i].cellnodeposition[1][1]*CELL_SCALE;
		const int normal_middle[2] = {lines[i].normal[0][0], lines[i].normal[0][1]};
		const int normal_delta[2]  = {lines[i].normal[1][0], lines[i].normal[1][1]};
		BAS_UseColour(128, 128, 128);
		SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
		BAS_UseColour(NORMAL_COLOUR[0], NORMAL_COLOUR[1], NORMAL_COLOUR[2]);
		SDL_RenderDrawLine(renderer,
			normal_middle[0], normal_middle[1],
			normal_middle[0]-normal_delta[1], normal_middle[1]+normal_delta[0]
		);
	}
}

static void
BAS_DrawThings(void)
{
	register int i;
	SDL_Rect rectangle;
	for (i = 0; i < thing_count; i++)
	{
		/* Outer shade */
		rectangle.x = things[i].thingposition[0];
		rectangle.y = things[i].thingposition[1];
		rectangle.w = THING_SCALE;
		rectangle.h = THING_SCALE;
		BAS_UseColour(0, 0, 144);
		SDL_RenderFillRect(renderer, &rectangle);
		/* Inner shade */
		rectangle.x += 1;
		rectangle.y += 1;
		rectangle.w -= 2;
		rectangle.h -= 2;
		BAS_UseColour(0, 255, 0);
		SDL_RenderFillRect(renderer, &rectangle);
	}
}

static void
BAS_DrawStatusline(void)
{
	SDL_Rect rectangle;
	rectangle.x = 0;
	rectangle.y = WINDOW_HEIGHT-32;
	SDL_QueryTexture(statuslinetexture[0], NULL, NULL, &rectangle.w, &rectangle.h);
	SDL_RenderCopy(renderer, statuslinetexture[0], NULL, &rectangle);
	rectangle.y = WINDOW_HEIGHT-16;
	SDL_QueryTexture(statuslinetexture[1], NULL, NULL, &rectangle.w, &rectangle.h);
	SDL_RenderCopy(renderer, statuslinetexture[1], NULL, &rectangle);
}

/*
 * ----------------
 * Help tool.
 * ----------------
 */
static SDL_Texture* helpme_textureauthor = NULL;
static SDL_Texture* helpme_textblock[8]  = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
static inline void
helpme_resetstate(void)
{
	SDL_SetCursor(cursorheap[CURSOR_ARROW]);
}
static void
helpme_createtextures(void)
{
	/*
	 * If helpme_textureauthor is not a valid texture, we can safely assume other
	 * textures are also invalid.
	 */
	if (!helpme_textureauthor)
	{
		helpme_textureauthor = BAS_CreateTextTexture(font_default, "author ★ Aleksandar Urošević, 2019.");
		helpme_textblock[0] = BAS_CreateTextTextureBlended(font_textinput, "Basilisk 0");
		helpme_textblock[1] = BAS_CreateTextTextureBlended(font_textinput, "----------------");
		helpme_textblock[2] = BAS_CreateTextTextureBlended(font_textinput, "F1 - help screen;");
		helpme_textblock[3] = BAS_CreateTextTextureBlended(font_textinput, "F2 - room placing tool;");
		helpme_textblock[4] = BAS_CreateTextTextureBlended(font_textinput, "F3 - thing editing tool;");
		helpme_textblock[5] = BAS_CreateTextTextureBlended(font_textinput, "F5 - export world plan.");
		helpme_textblock[6] = BAS_CreateTextTextureBlended(font_textinput, "");
		helpme_textblock[7] = BAS_CreateTextTextureBlended(font_textinput, "Have a nice day.");
	}
}
static void
helpme_destroytextures(void)
{
	SDL_DestroyTexture(helpme_textblock[0]);
	SDL_DestroyTexture(helpme_textblock[1]);
	SDL_DestroyTexture(helpme_textblock[2]);
	SDL_DestroyTexture(helpme_textblock[3]);
	SDL_DestroyTexture(helpme_textblock[4]);
	SDL_DestroyTexture(helpme_textblock[5]);
	SDL_DestroyTexture(helpme_textblock[6]);
	SDL_DestroyTexture(helpme_textblock[7]);
	SDL_DestroyTexture(helpme_textureauthor);
	helpme_textureauthor = NULL;
}
static void
BAS_Tool_HelpMe(SDL_Event e, int mx, int my, int special)
{
	switch(special)
	{
	case TOOL_SPECIAL_RESETSTATE:
		helpme_resetstate();
		return;
	case TOOL_SPECIAL_BEGIN:
		helpme_createtextures();
		return;
	case TOOL_SPECIAL_STOP:
		helpme_destroytextures();
		return;
	}
}
static void
BAS_Tool_HelpMe_Draw(int mx, int my)
{
	int i;
	const int screen_w = 800;
	const int screen_h = 500;
	SDL_Rect rectangle;
	rectangle.w = screen_w;
	rectangle.h = screen_h;
	rectangle.x = WINDOW_WIDTH/2-screen_w/2;
	rectangle.y = WINDOW_HEIGHT/2-screen_h/2;
	/* Title image */
	SDL_RenderCopy(renderer, basilisk_texture, NULL, &rectangle);
	/* Outline */
	BAS_UseColour(255, 255, 255);
	SDL_RenderDrawRect(renderer, &rectangle);
	/* Text on the title image */
	rectangle.x += 1;
	rectangle.y += 1;
	for (i = 0; i < 8; i++)
	{
		SDL_QueryTexture(helpme_textblock[i], NULL, NULL, &rectangle.w, &rectangle.h);
		SDL_RenderCopy(renderer, helpme_textblock[i], NULL, &rectangle);
		rectangle.y += 24;
	}
	/* Black strip */
	rectangle.x = 1+WINDOW_WIDTH/2-screen_w/2;
	rectangle.y = WINDOW_HEIGHT/2+(screen_h*3.0)/7.0;
	rectangle.h = 12;
	rectangle.w = screen_w-2;
	BAS_UseColour(0, 0, 0);
	SDL_RenderFillRect(renderer, &rectangle);
	/* Author text */
	rectangle.x += rectangle.w;
	SDL_QueryTexture(helpme_textureauthor, NULL, NULL, &rectangle.w, &rectangle.h);
	rectangle.x -= rectangle.w+12;
	SDL_RenderCopy(renderer, helpme_textureauthor, NULL, &rectangle);
}

/*
 * ----------------
 * Tool for placing rooms.
 * ----------------
 */
static inline void
drawroom_resetstate(void)
{
	SDL_SetCursor(cursorheap[CURSOR_ARROW]);
}
static void
BAS_Tool_DrawRoom(SDL_Event e, int mx, int my, int special)
{
	switch(special)
	{
	case TOOL_SPECIAL_RESETSTATE:
		drawroom_resetstate();
	case TOOL_SPECIAL_STOP:
		return;
	}
	if ((e.type == SDL_KEYDOWN         && e.key.keysym.sym == SDLK_SPACE)
	 || (e.type == SDL_MOUSEBUTTONDOWN && e.button.button  == SDL_BUTTON_LEFT))
	{
		int cx, cy;
		BAS_ClosestCellPosition(mx, my, &cx, &cy);
		if (BAS_Room_Create(cx, cy))
		{
			BAS_PushStatusAndWriteWarning("Selected room already exists.");
		}
		else
		{
			BAS_RecalculateLines();
		}
	}
	else if ((e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_DELETE)
	 || (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT))
	{
		register int i;
		int cx, cy;
		BAS_ClosestCellPosition(mx, my, &cx, &cy);
		if ((i = BAS_FindRoom(cx, cy)) != BAS_NO_SUCH_ROOM)
		{
			register int j;
			for (j = i; j < room_count; j++)
			{
				rooms[j] = rooms[j+1];
			}
			room_count--;
			BAS_RecalculateLines();
		}
		else
		{
			BAS_PushStatusAndWriteWarning("No room under the cursor to delete!");
		}
	}
}
/* While the room tool is used, draw the room in the cell that's under the mouse */
static void
BAS_Tool_DrawRoom_Draw(int mx, int my)
{
	SDL_Rect rectangle;
	const int activeroomalpha = 255*fabsf(sinf(SDL_GetTicks()/300.0f));
	BAS_DrawCrosshair();
	BAS_SnapToClosestCell(&mx, &my);
	rectangle.x = mx;
	rectangle.y = my;
	rectangle.w = CELL_SCALE;
	rectangle.h = CELL_SCALE;
	BAS_UseColourAlpha(255, 0, 0, activeroomalpha);
	SDL_RenderFillRect(renderer, &rectangle);
}

/*
 * Export the current plan state to a file.
 * Returns 0 on success, 1 on error.
 * The file format is very simple, here is how it looks (words with $ are variables):
 *
 * Basilisk $version
 */
static int
BAS_ExportPlan(const char path[96])
{
	FILE *output;
	WRITE_I("Writing to file...");
	output = fopen(path, "w");
	if (!output)
	{
		WRITE_E("Failed to open file for writing!");
		return 1;
	}
	fprintf(output, "Basilisk 0\n");
	return 0;
}

/*
 * ----------------
 * Thing placing tool
 * ----------------
 */
static int panel_width = 225;
static int thing_seeinfo = 0;
static int thing_selected = BAS_NO_SUCH_THING;
static int thingtool_updatecursor = 1;
static SDL_Texture* thing_infos[32] = {NULL};
static inline void
thingtool_resetstate(void)
{
	thing_seeinfo  = 0;
	thing_selected = 0;
}
static void
thingtool_updateinfopanel(const int thingindex)
{
	char buffer[32];
	SDL_DestroyTexture(thing_infos[0]);
	snprintf(buffer, 32, "Thing index %d.", thingindex);
	thing_infos[0] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "----------------");
	thing_infos[1] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "Structure data:");
	thing_infos[2] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "struct BAS_Thing");
	thing_infos[3] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "{");
	thing_infos[4] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "  uint64_t flags = %ld;", things[thingindex].flags);
	thing_infos[5] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "  int thingposition[0] = %d;", things[thingindex].thingposition[0]);
	thing_infos[6] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "  int thingposition[1] = %d;", things[thingindex].thingposition[1]);
	thing_infos[7] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "  int type = %d;", things[thingindex].type);
	thing_infos[8] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "  int facing = %d;", things[thingindex].facing);
	thing_infos[9] = BAS_CreateTextTexture(font_default, buffer);
	snprintf(buffer, 32, "}");
	thing_infos[10] = BAS_CreateTextTexture(font_default, buffer);
}
static void
BAS_Tool_ThingPlace(SDL_Event e, int mx, int my, int special)
{
	switch(special)
	{
	case TOOL_SPECIAL_RESETSTATE:
		thingtool_resetstate();
		return;
	case TOOL_SPECIAL_BEGIN:
		SDL_ShowCursor(SDL_DISABLE);
		return;
	case TOOL_SPECIAL_STOP:
		SDL_ShowCursor(SDL_ENABLE);
		return;
	}
	if (e.type == SDL_KEYDOWN)
	{
		switch(e.key.keysym.sym)
		{
			case SDLK_e:
				thing_seeinfo = !thing_seeinfo;
				break;
			/* Control selected thing's facing direction. */
			case SDLK_UP:    if (thing_selected != BAS_NO_SUCH_THING) { things[thing_selected].facing = 1; thingtool_updateinfopanel(thing_selected); } break;
			case SDLK_LEFT:  if (thing_selected != BAS_NO_SUCH_THING) { things[thing_selected].facing = 2; thingtool_updateinfopanel(thing_selected); } break;
			case SDLK_DOWN:  if (thing_selected != BAS_NO_SUCH_THING) { things[thing_selected].facing = 3; thingtool_updateinfopanel(thing_selected); } break;
			case SDLK_RIGHT: if (thing_selected != BAS_NO_SUCH_THING) { things[thing_selected].facing = 0; thingtool_updateinfopanel(thing_selected); } break;
		}
	}
	else if (e.type == SDL_MOUSEBUTTONDOWN)
	{
		if (e.button.button == SDL_BUTTON_LEFT)
		{
			int selected;
			int x, y;
			x = mx;
			y = my;
			BAS_SnapToClosestCell_Custom(&x, &y, THING_SCALE);
			selected = BAS_FindThing(x, y);
			if (selected != BAS_NO_SUCH_THING)
			{
				thing_selected = selected;
				thing_seeinfo  = 1;
				thingtool_updateinfopanel(selected);
			}
			else
			{
				things[thing_count].thingposition[0] = x;
				things[thing_count].thingposition[1] = y;
				things[thing_count].facing = thing_count%4;
				thing_seeinfo  = 1;
				thing_selected = thing_count;
				thingtool_updateinfopanel(thing_selected);
				thing_count++;
			}
		}
	}
	else if (e.type == SDL_MOUSEMOTION)
	{
		if (e.motion.x < panel_width && thing_seeinfo)
		{
			SDL_ShowCursor(SDL_ENABLE);
			thingtool_updatecursor = 0;
		}
		else
		{
			SDL_ShowCursor(SDL_DISABLE);
			thingtool_updatecursor = 1;
		}
	}
}
static int thingtool_cursorposition[2];
static void
BAS_Tool_ThingPlace_Draw(int mx, int my)
{
	SDL_Rect rectangle;
	int i;
	int oldmx, oldmy;
	const int activethingalpha = 255*fabsf(sinf(SDL_GetTicks()/100.0f));
	oldmx = mx;
	oldmy = my;
	BAS_SnapToClosestCell_Custom(&mx, &my, THING_SCALE);
	rectangle.x = mx;
	rectangle.y = my;
	rectangle.w = THING_SCALE;
	rectangle.h = THING_SCALE;
	BAS_UseColourAlpha(255, 255, 0, activethingalpha);
	SDL_RenderFillRect(renderer, &rectangle);
	/* Render the "big" outline for the selected thing. */
	if (thing_selected != BAS_NO_SUCH_THING)
	{
		const int activethingalpha = 255*(fabsf(sinf(SDL_GetTicks()/100.0f))/2.0f+0.5f);
		rectangle.x = things[thing_selected].thingposition[0]-4;
		rectangle.y = things[thing_selected].thingposition[1]-4;
		rectangle.w = THING_SCALE+8;
		rectangle.h = THING_SCALE+8;
		BAS_UseColourAlpha(0, 255, 0, activethingalpha);
		SDL_RenderDrawRect(renderer, &rectangle);
	}
	/* Render cursor. */
	if (thingtool_updatecursor)
	{
		thingtool_cursorposition[0] = oldmx;
		thingtool_cursorposition[1] = oldmy;
		SDL_GetMouseState(&thingtool_cursorposition[0], &thingtool_cursorposition[1]);
	}
	BAS_DrawCrosshair_Small(thingtool_cursorposition[0], thingtool_cursorposition[1]);
	/* Thing info editor. */
	if (thing_seeinfo)
	{
		const int facingpanel_scale = panel_width/2;
		int facingpanel_line[2][2];
		/* Panel */
		rectangle.x = 0;
		rectangle.y = 0;
		rectangle.w = panel_width;
		rectangle.h = WINDOW_HEIGHT;
		BAS_UseColour(0, 0, 0);
		SDL_RenderFillRect(renderer, &rectangle);
		/* Panel gradient */
		BAS_UseColourAlpha(255, 255, 255, 60);
		for (i = 1; i < 50; i++)
		{
			float y = WINDOW_HEIGHT-tan(i/50.0)*50;
			SDL_RenderDrawLine(renderer, 0, y, rectangle.w, y);
		}
		/* Thing info text */
		rectangle.x = 0;
		rectangle.y = 0;
		for (i = 0; i < 11; i++)
		{
			SDL_QueryTexture(thing_infos[i], NULL, NULL, &rectangle.w, &rectangle.h);
			SDL_RenderCopy(renderer, thing_infos[i], NULL, &rectangle);
			rectangle.y += rectangle.h;
		}
		rectangle.y += rectangle.h;
		/* Thing facing direction */
		rectangle.w = rectangle.h = facingpanel_scale;
		BAS_UseColour(20, 60, 20);
		rectangle.w += 4;
		rectangle.h += 4;
		SDL_RenderFillRect(renderer, &rectangle);
		rectangle.w -= 4;
		rectangle.h -= 4;
		BAS_UseColour(30, 200, 30);
		SDL_RenderFillRect(renderer, &rectangle);
		facingpanel_line[0][0] = facingpanel_scale/2;
		facingpanel_line[0][1] = rectangle.y+facingpanel_scale/2;
		switch (things[thing_selected].facing)
		{
			case 0:
				facingpanel_line[1][0] = facingpanel_scale;
				facingpanel_line[1][1] = facingpanel_line[0][1];
				break;
			case 1:
				facingpanel_line[1][0] = facingpanel_line[0][0];
				facingpanel_line[1][1] = facingpanel_line[0][1]-facingpanel_scale/2;
				break;
			case 2:
				facingpanel_line[1][0] = -facingpanel_scale/2;
				facingpanel_line[1][1] = facingpanel_line[0][1];
				break;
			case 3:
				facingpanel_line[1][0] = facingpanel_line[0][0];
				facingpanel_line[1][1] = facingpanel_line[0][1]+facingpanel_scale/2;
				break;
		}
		BAS_UseColour(255, 255, 255);
		SDL_RenderDrawLine(renderer, facingpanel_line[0][0], facingpanel_line[0][1], facingpanel_line[1][0], facingpanel_line[1][1]);
	}
}

/*
 * ----------------
 * Export plan tool
 * ----------------
 */
/* Define the indices for the texture array. */
#define INPUT 0
#define LABEL 1
#define ADDITIONAL 2
#define EXPORTED 3
#define DEFAULT_EXPORT_FILE "./plans/t"
static char exportplan_filepath[96]  = DEFAULT_EXPORT_FILE;
static int exportplan_filepathlength = strlen(DEFAULT_EXPORT_FILE);
static int exportplan_cursor         = 8;
static int exportplan_planexported   = 0;
static SDL_Texture* exportplan_textures[4] = {NULL, NULL, NULL, NULL};
static const int CURSOR_BLINK_INTERVAL = 512;
static void
exportplan_updateinputtexture(void)
{
	char inputtext[96];
	strcpy(inputtext, exportplan_filepath);
	if ((SDL_GetTicks() % CURSOR_BLINK_INTERVAL) > CURSOR_BLINK_INTERVAL/2)
	{
		char buf[96] = "\0";
		/* Don't copy garbage if the cursor is at the end of the input string... */
		if (exportplan_cursor != exportplan_filepathlength)
		{
			strcpy(buf, inputtext+exportplan_cursor+1);
		}
		strcpy(inputtext+exportplan_cursor, "_");
		strcat(inputtext, buf);
	}
	if (exportplan_textures[INPUT])
	{
		SDL_DestroyTexture(exportplan_textures[INPUT]);
	}
	exportplan_textures[INPUT] = BAS_CreateTextTexture(font_textinput, inputtext);
}
static void
exportplan_begin(void)
{
	exportplan_textures[LABEL]      = BAS_CreateTextTexture(font_textinput, "Output file path: ");
	exportplan_textures[ADDITIONAL] = BAS_CreateTextTexture(font_textinput, "Insert the file's name and press RETURN to write...");
	exportplan_textures[EXPORTED]   = BAS_CreateTextTexture(font_textinput, "The file has been written.");
	exportplan_updateinputtexture();
}
static void
exportplan_stop(void)
{
	SDL_DestroyTexture(exportplan_textures[3]);
	exportplan_textures[3] = NULL;
	SDL_DestroyTexture(exportplan_textures[2]);
	exportplan_textures[2] = NULL;
	SDL_DestroyTexture(exportplan_textures[1]);
	exportplan_textures[1] = NULL;
	SDL_DestroyTexture(exportplan_textures[0]);
	exportplan_textures[0] = NULL;
}
static inline void
exportplan_resetstate(void)
{
	SDL_SetCursor(cursorheap[CURSOR_ARROW]);
	exportplan_planexported = 0;
}
static void
BAS_Tool_ExportPlan(SDL_Event e, int mx, int my, int special)
{
	register int i;
	switch(special)
	{
	case TOOL_SPECIAL_RESETSTATE:
		exportplan_resetstate();
		return;
	case TOOL_SPECIAL_BEGIN:
		exportplan_begin();
		return;
	case TOOL_SPECIAL_STOP:
		exportplan_stop();
		return;
	}
	/*
	 * Text editor.
	 * Left/right arrow move the cursor.
	 * Backspace deletes the previous character.
	 * Delete deletes the current character.
	 * Valid characters are [a-z],./
	 */
	if (e.type == SDL_KEYDOWN)
	{
		switch (e.key.keysym.sym)
		{
			case SDLK_RETURN:
				if (BAS_ExportPlan(exportplan_filepath))
				{
					BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_ERROR, "File export error", "The file was not written.");
					exportplan_planexported = 0;
				}
				else
				{
					BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_INFO, "File export", "The file was successfully written.");
					exportplan_planexported = 1;
				}
				break;
			case SDLK_LEFT:
				if (exportplan_cursor > 0)
				{
					exportplan_cursor--;
					exportplan_updateinputtexture();
				}
				break;
			case SDLK_RIGHT:
				if (exportplan_cursor < exportplan_filepathlength)
				{
					exportplan_cursor++;
					exportplan_updateinputtexture();
				}
				break;
			case SDLK_HOME:
				if (exportplan_cursor != 0)
				{
					exportplan_cursor = 0;
					exportplan_updateinputtexture();
				}
				break;
			case SDLK_END:
				if (exportplan_cursor != exportplan_filepathlength)
				{
					exportplan_cursor = exportplan_filepathlength;
					exportplan_updateinputtexture();
				}
				break;
			case SDLK_BACKSPACE:
				if (exportplan_cursor > 0)
				{
					for (i = exportplan_cursor-1; i < exportplan_filepathlength+1; i++)
					{
						exportplan_filepath[i] = exportplan_filepath[i+1];
					}
					exportplan_cursor--;
					exportplan_filepathlength--;
					exportplan_updateinputtexture();
				}
				break;
			case SDLK_DELETE:
				if (exportplan_cursor >= 0 && exportplan_filepath[exportplan_cursor] != '\0')
				{
					for (i = exportplan_cursor; i < exportplan_filepathlength+1; i++)
					{
						exportplan_filepath[i] = exportplan_filepath[i+1];
					}
					exportplan_filepathlength--;
					exportplan_updateinputtexture();
				}
				break;
			case SDLK_a: case SDLK_b: case SDLK_c: case SDLK_d: case SDLK_e:
			case SDLK_f: case SDLK_g: case SDLK_h: case SDLK_i: case SDLK_j:
			case SDLK_k: case SDLK_l: case SDLK_m: case SDLK_n: case SDLK_o:
			case SDLK_p: case SDLK_q: case SDLK_r: case SDLK_s: case SDLK_t:
			case SDLK_u: case SDLK_v: case SDLK_w: case SDLK_x: case SDLK_y:
			case SDLK_z:
			case SDLK_0: case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
			case SDLK_5: case SDLK_6: case SDLK_7: case SDLK_8: case SDLK_9:
			case SDLK_PERIOD: case SDLK_COMMA: case SDLK_SLASH:
				exportplan_filepath[exportplan_cursor] = e.key.keysym.sym;
				exportplan_cursor++;
				exportplan_filepathlength = strlen(exportplan_filepath);
				exportplan_updateinputtexture();
		}
	}
}
static void
BAS_Tool_ExportPlan_Draw(int mx, int my)
{
	SDL_Rect rectangle;
	int screen_left, screen_top, temp;
	exportplan_updateinputtexture();
	/* Screen */
	rectangle.w = 800;
	rectangle.h = 128;
	screen_left = WINDOW_WIDTH/2-rectangle.w/2;
	rectangle.x = screen_left;
	screen_top  = WINDOW_HEIGHT/2-rectangle.h/2;
	rectangle.y = screen_top;
	BAS_UseColourAlpha(0, 0, 0, 196);
	SDL_RenderFillRect(renderer, &rectangle);
	BAS_UseColour(255, 255, 255);
	SDL_RenderDrawRect(renderer, &rectangle);
	/* Input text label */
	rectangle.x = screen_left;
	SDL_QueryTexture(exportplan_textures[LABEL], NULL, NULL, &rectangle.w, &rectangle.h);
	SDL_RenderCopy(renderer, exportplan_textures[LABEL], NULL, &rectangle);
	/* Input text */
	rectangle.x = temp = rectangle.x+rectangle.w;
	SDL_QueryTexture(exportplan_textures[INPUT], NULL, NULL, &rectangle.w, &rectangle.h);
	SDL_RenderCopy(renderer, exportplan_textures[INPUT], NULL, &rectangle);
	/* Additinal text */
	rectangle.x = screen_left;
	rectangle.y += rectangle.h;
	SDL_QueryTexture(exportplan_textures[ADDITIONAL], NULL, NULL, &rectangle.w, &rectangle.h);
	SDL_RenderCopy(renderer, exportplan_textures[ADDITIONAL], NULL, &rectangle);
	/* Additinal text */
	if (exportplan_planexported)
	{
		rectangle.x = screen_left;
		rectangle.y += rectangle.h;
		SDL_QueryTexture(exportplan_textures[EXPORTED], NULL, NULL, &rectangle.w, &rectangle.h);
		SDL_RenderCopy(renderer, exportplan_textures[EXPORTED], NULL, &rectangle);
	}
}

#define CHECKSDL(check) if (check) { WRITE_E(SDL_GetError()); return 1; }
int
main(void)
{
	int running, havefocus, mousemotion;
	executionjump currentjump, previousjump;
	drawexectionjump drawjump;
	SDL_Event e;
	SDL_Surface *surface;
	/* Beginning */
	WRITE_I("This is Basilisk ("BASILISK_VERSION").");
	WRITE_I("Call SDL_Init.");
	CHECKSDL(SDL_Init(SDL_INIT_EVERYTHING));
	WRITE_I("Call TTF_Init.");
	CHECKSDL(TTF_Init());
	WRITE_I("Opening window.");
	window = SDL_CreateWindow(
		"_BASILISK_",
		SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED,
		WINDOW_WIDTH,
		WINDOW_HEIGHT,
		0
	);
	CHECKSDL(!window);
	WRITE_I("Creating renderer.");
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
	CHECKSDL(!renderer);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	/* Persistent data */
	WRITE_I("Load persistent data into memory.");
	font_default                  = TTF_OpenFont(FONT_PATH, 12);
	font_textinput                = TTF_OpenFont(FONT_PATH, 24);
	cursorheap[CURSOR_ARROW]      = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
	cursorheap[CURSOR_CROSSHAIR]  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
	cursorheap[CURSOR_HAND]       = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
	cursorheap[CURSOR_CROSSBONES] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NO);
	surface = IMG_Load("data/bas.tga");
	basilisk_texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	/* Loop */
	running     = 1;
	havefocus   = 1;
	mousemotion = 0;
	currentjump = previousjump = &BAS_Tool_HelpMe;
	drawjump    = BAS_Tool_HelpMe_Draw;
	currentjump(e, -1, -1, TOOL_SPECIAL_BEGIN);
	BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_INFO, "All okay.", "F1 - help; F2 - room tool; F5 - export world plan.");
	WRITE_I("All okay.");
	while (running)
	{
		int mx, my;
		int special = TOOL_SPECIAL_VOID;
		mousemotion = 0;
		while (SDL_PollEvent(&e))
		{
			/* Quit. */
			if (e.type == SDL_QUIT)
			{
				running = 0;
			}
			/* Handle window events such as minimisation, focus loss/gain, etc. */
			else if (e.type == SDL_WINDOWEVENT)
			{
				switch (e.window.event)
				{
				case SDL_WINDOWEVENT_SHOWN:
				case SDL_WINDOWEVENT_FOCUS_GAINED:
					delayperframe = DELAY_PER_FRAME_DEFAULT;
					havefocus = 1;
					break;
				case SDL_WINDOWEVENT_HIDDEN:
				case SDL_WINDOWEVENT_MINIMIZED:
				case SDL_WINDOWEVENT_FOCUS_LOST:
					delayperframe = DELAY_PER_FRAME_ABSENT;
					havefocus = 0;
				}
			}
			else if (e.type == SDL_MOUSEMOTION)
			{
				mousemotion = 1;
			}
			/*
			 * Change the active tool.
			 * If the active tool changes this frame, set the default cursor.
			 */
			if (e.type == SDL_KEYDOWN)
			{
				switch(e.key.keysym.sym)
				{
				case SDLK_ESCAPE:
					special = TOOL_SPECIAL_RESETSTATE;
					break;
				case SDLK_F1:
					currentjump(e, mx, my, TOOL_SPECIAL_RESETSTATE);
					currentjump = &BAS_Tool_HelpMe;
					drawjump = &BAS_Tool_HelpMe_Draw;
					BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_INFO, "BAS_Tool_HelpMe", "...");
					break;
				case SDLK_F2:
					currentjump(e, mx, my, TOOL_SPECIAL_RESETSTATE);
					currentjump = &BAS_Tool_DrawRoom;
					drawjump = BAS_Tool_DrawRoom_Draw;
					BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_INFO, "Room tool is now being used.", "Use the mouse to place rooms on the grid.");
					break;
				case SDLK_F3:
					currentjump(e, mx, my, TOOL_SPECIAL_RESETSTATE);
					currentjump = &BAS_Tool_ThingPlace;
					drawjump = BAS_Tool_ThingPlace_Draw;
					BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_INFO, "Thing placing tool is now being used.", "$instructions");
					break;
				case SDLK_F5:
					currentjump(e, mx, my, TOOL_SPECIAL_RESETSTATE);
					currentjump = &BAS_Tool_ExportPlan;
					drawjump = &BAS_Tool_ExportPlan_Draw;
					BAS_PushStatus(BAS_STATUSMESSAGE_TYPE_INFO, "Exporting world plan.", "Choose the options.");
					break;
				}
			}
			if (currentjump != previousjump)
			{
				previousjump(e, mx, my, TOOL_SPECIAL_STOP);
				currentjump (e, mx, my, TOOL_SPECIAL_BEGIN);
				previousjump = currentjump;
				SDL_SetCursor(cursorheap[CURSOR_ARROW]);
			}
			/* Handle the tool. */
			SDL_GetMouseState(&mx, &my);
			currentjump(e, mx, my, special);
		}
		/* To make motion smooth, delay should be minimised when we're moving the mouse. */
		if (havefocus)
		{
			if (mousemotion) { delayperframe = DELAY_PER_FRAME_MOTION;  }
			else             { delayperframe = DELAY_PER_FRAME_DEFAULT; }
		}
		/* Draw */
		BAS_UseColour(0, 0, 20);
		BAS_Clear;
		BAS_DrawGrid();
		BAS_DrawRooms();
		BAS_DrawLines();
		BAS_DrawThings();
		if (drawjump)
		{
			drawjump(mx, my);
		}
		BAS_DrawStatusline();
		BAS_Present;
		/* And wait some time... */
		SDL_Delay(delayperframe);
	}
	/* End */
	WRITE_I("Freeing memory now.");
	currentjump(e, 0, 0, TOOL_SPECIAL_STOP);
	SDL_DestroyTexture(basilisk_texture);
	SDL_FreeCursor(cursorheap[CURSOR_CROSSBONES]);
	SDL_FreeCursor(cursorheap[CURSOR_HAND]);
	SDL_FreeCursor(cursorheap[CURSOR_CROSSHAIR]);
	SDL_FreeCursor(cursorheap[CURSOR_ARROW]);
	SDL_DestroyWindow(window);
	SDL_DestroyRenderer(renderer);
	TTF_CloseFont(font_textinput);
	TTF_CloseFont(font_default);
	TTF_Quit();
	SDL_Quit();
	WRITE_I("Goodbye.");
	return 0;
}

