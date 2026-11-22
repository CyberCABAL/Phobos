// Smooth ground lighting
//
// Vanilla paints every ground tile with one tinted palette and one brightness, both taken
// from the tile's own cell and sampled only at the cell's centre (CellClass::DrawTile at
// 0x480350 calls the tile painter at 0x547CF0). Light posts therefore show up as stepped
// diamonds and tints change abruptly at cell edges.
//
// When Phobos::Config::SmoothGroundLighting is on, this re-implements the painter for
// DrawTile. Every pixel is blended from the four nearest cells (bilinear between cell
// centres), each looked up in that cell's own palette at that cell's own brightness, so
// both tint and brightness change smoothly. The per-pixel ABuffer multiplier (AlphaImage
// lighting) is applied exactly like vanilla. Tiles whose neighbourhood is uniformly lit
// are left to the vanilla painter, since the result would be identical.
//
// Cliffs: cells too far apart in height are not blended across the ground plane. Their
// share of the blend goes to the cell beside them instead, so plateau rims and cliff feet
// stay smooth along the edge. Tiles next to a cliff light their pixels by the surface that
// is visible there (see CliffShader), because cliff art is often drawn by a tile that
// doesn't border the face it shows.

#include <Phobos.h>

#include <AlphaLightingRemapClass.h>
#include <CellClass.h>
#include <ConvertClass.h>
#include <Drawing.h>
#include <IsometricTileTypeClass.h>
#include <MapClass.h>
#include <Surface.h>

#include <Utilities/Macro.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace SmoothGroundLighting
{
	// One tile block of a TMP file as the game keeps it in memory.
	struct TmpBlock
	{
		int X;
		int Y;
		int ExtraOffset;  // relative to the block
		int ZOffset;      // relative to the block
		int ExtraZOffset; // relative to the block
		int ExtraX;
		int ExtraY;
		int ExtraWidth;
		int ExtraHeight;
		DWORD Flags;      // 1 = has extra image, 2 = has Z data
		BYTE Unused[0x0C];

		const BYTE* Data(int offset) const
		{
			return reinterpret_cast<const BYTE*>(this) + offset;
		}
	};
	static_assert(sizeof(TmpBlock) == 0x34, "TMP block header must be 0x34 bytes");

	struct TmpFile
	{
		int BlocksX;
		int BlocksY;
		int BlockWidth;
		int BlockHeight;
		TmpBlock* Blocks[1]; // BlocksX * BlocksY entries, may be null
	};

	constexpr int TileWidth = 60;
	constexpr int TileHeight = 30;
	constexpr int TileRows = 29; // the 30th row of the diamond is empty
	constexpr int LevelHeight = 15; // screen pixels per height level
	constexpr int MaxLevelDifference = 2; // cells further apart are not blended across the ground plane

	enum FlagBits : DWORD
	{
		HasExtra = 0x1,
		HasZData = 0x2,
	};

	// One cell's contribution: its palette and its shade row (already shifted into the remap table).
	struct Sample
	{
		const WORD* Palette;
		int Row;

		bool operator==(const Sample& other) const
		{
			return this->Palette == other.Palette && this->Row == other.Row;
		}

		bool operator!=(const Sample& other) const
		{
			return !(*this == other);
		}
	};

	// Four samples and how to combine them. Weights are given in the same slot order.
	struct Quad
	{
		Sample Slot[4];
		bool Uniform; // all used slots are the same, no blending needed
		bool Split;   // slot 3 is missing: give its weight half to slot 1 and half to slot 2
	};

	// Blend weights of one pixel, in 32ths, and which quad they apply to.
	struct PixelWeights
	{
		BYTE Quadrant;
		BYTE W[4];
	};

	static int RowStart[TileRows];
	static int RowWidth[TileRows];
	static int RowOffset[TileRows];
	static int ColumnTop[TileWidth];
	static int ColumnBottom[TileWidth];
	static PixelWeights DiamondWeights[TileRows][TileWidth];
	static bool TablesReady = false;

	// Bilinear weights in 32ths from the distance toward the X side (a) and the Y side (b), each 0-32.
	// Only the corner weight is rounded, the others are derived from it, so none can go negative.
	static inline void Bilinear(int a, int b, BYTE* weights)
	{
		const int corner = (a * b + 16) >> 5;
		weights[3] = static_cast<BYTE>(corner);
		weights[1] = static_cast<BYTE>(a - corner);
		weights[2] = static_cast<BYTE>(b - corner);
		weights[0] = static_cast<BYTE>(32 - a - b + corner);
	}

	// Map-space offset of a pixel from the cell centre along X (s) and Y (t), scaled so that
	// +-60 is the diamond's edge. Pixel x/y are relative to the top-left of the 60x30 diamond.
	// A cell is 60 px wide and 30 px tall, map X runs to screen right-down, map Y to left-down.
	static inline void MapOffset(int pixelX, int pixelY, int& s, int& t)
	{
		// Half pixels relative to the top vertex, so the pixel centre is an integer.
		const int dx2 = 2 * pixelX + 1 - TileWidth;
		const int dy2 = 2 * pixelY + 1;
		s = dx2 + 2 * dy2 - 60;
		t = 2 * dy2 - dx2 - 60;
	}

	// Distance toward the neighbour in 32ths of a cell, at most half a cell (16).
	static inline int Toward(int offset)
	{
		return std::min(16, (std::abs(offset) * 8 + 15) / 30);
	}

	static PixelWeights ComputeDiamondWeights(int pixelX, int pixelY)
	{
		int s, t;
		MapOffset(pixelX, pixelY, s, t);

		PixelWeights weights;
		weights.Quadrant = static_cast<BYTE>((s > 0 ? 1 : 0) | (t > 0 ? 2 : 0));
		Bilinear(Toward(s), Toward(t), weights.W);
		return weights;
	}

	static void InitTables()
	{
		if (TablesReady)
			return;

		int offset = 0;

		for (int column = 0; column < TileWidth; ++column)
		{
			ColumnTop[column] = TileRows;
			ColumnBottom[column] = -1;
		}

		for (int row = 0; row < TileRows; ++row)
		{
			// Same diamond as the game's mask at 0x7EC4C8 and its row offsets at 0x7EC450.
			RowStart[row] = row < 15 ? 28 - 2 * row : 2 * (row - 14);
			RowWidth[row] = row < 15 ? 4 + 4 * row : 60 - 4 * (row - 14);
			RowOffset[row] = offset;
			offset += RowWidth[row];

			for (int column = 0; column < TileWidth; ++column)
				DiamondWeights[row][column] = ComputeDiamondWeights(column, row);

			for (int column = RowStart[row]; column < RowStart[row] + RowWidth[row]; ++column)
			{
				ColumnTop[column] = std::min(ColumnTop[column], row);
				ColumnBottom[column] = std::max(ColumnBottom[column], row);
			}
		}

		TablesReady = true;
	}

	// The blend packs a 16-bit colour as (G << 16) | R | B so four colours can be
	// weighted with four multiplies. Only the two 16-bit layouts the game uses are supported.
	struct PixelFormat
	{
		DWORD SpreadMask;
		DWORD Bias;
	};

	static bool GetPixelFormat(PixelFormat& format)
	{
		const int red = (0xFF >> Drawing::RedShiftRight) << Drawing::RedShiftLeft;
		const int green = (0xFF >> Drawing::GreenShiftRight) << Drawing::GreenShiftLeft;
		const int blue = (0xFF >> Drawing::BlueShiftRight) << Drawing::BlueShiftLeft;

		if (red == 0xF800 && green == 0x07E0 && blue == 0x001F)
		{
			format.SpreadMask = 0x07E0F81F;
			format.Bias = 0x00200801 << 4; // half of the lowest bit of each channel, times 32
			return true;
		}

		if (red == 0x7C00 && green == 0x03E0 && blue == 0x001F)
		{
			format.SpreadMask = 0x03E07C1F;
			format.Bias = 0x00200401 << 4;
			return true;
		}

		return false;
	}

	static inline DWORD Spread(WORD color, DWORD mask)
	{
		return (color | (static_cast<DWORD>(color) << 16)) & mask;
	}

	static inline int IntensityToRow(int intensity)
	{
		// Same as the vanilla painter: 1000 -> 127, capped at 254.
		return std::min((std::max(intensity, 0) * 0x105) >> 11, 0xFE);
	}

	// A cell around the one being drawn.
	struct Neighbour
	{
		Sample Value;
		int Level;
		bool Exists; // on the map
		bool Valid;  // exists and has a usable palette
	};

	// The drawn cell and the cells around it. Every tile gathers the 3x3; tiles next to a
	// cliff gather the 7x7, since the cliff lookup can land on any cell of the 5x5 and needs
	// that cell's own neighbours.
	constexpr int GridRadius = 3;

	struct CellGrid
	{
		Neighbour Cells[2 * GridRadius + 1][2 * GridRadius + 1];

		Neighbour& At(int x, int y)
		{
			return this->Cells[y + GridRadius][x + GridRadius];
		}

		const Neighbour& At(int x, int y) const
		{
			return this->Cells[y + GridRadius][x + GridRadius];
		}

		// Close enough in height to the other cell to blend across the ground plane.
		static bool Plane(const Neighbour& cell, const Neighbour& other)
		{
			return cell.Valid && std::abs(cell.Level - other.Level) <= MaxLevelDifference;
		}
	};

	static bool GetSample(CellClass* pCell, int shadeCount, Sample& sample)
	{
		auto const pConvert = pCell->LightConvert;

		if (!pConvert || pConvert->ShadeCount != shadeCount || !pConvert->FullColorData)
			return false;

		sample.Palette = static_cast<const WORD*>(pConvert->FullColorData);
		sample.Row = IntensityToRow(static_cast<short>(pCell->Intensity_Terrain)) << 8;
		return true;
	}

	// Fills the cells from ring `from` to ring `to` around the centre.
	static void Gather(CellGrid& grid, CellClass* pCell, int shadeCount, int from, int to)
	{
		const Neighbour& centre = grid.At(0, 0);

		for (int y = -to; y <= to; ++y)
		{
			for (int x = -to; x <= to; ++x)
			{
				if (std::max(std::abs(x), std::abs(y)) < from)
					continue;

				Neighbour& cell = grid.At(x, y);
				cell.Value = centre.Value;
				cell.Level = centre.Level;
				cell.Exists = false;
				cell.Valid = false;

				const CellStruct coords { static_cast<short>(pCell->MapCoords.X + x), static_cast<short>(pCell->MapCoords.Y + y) };

				if (auto const pOther = MapClass::Instance.TryGetCellAt(coords))
				{
					cell.Exists = true;
					cell.Level = pOther->Level;
					cell.Valid = GetSample(pOther, shadeCount, cell.Value);
				}
			}
		}
	}

	// Whether any two neighbouring cells of the 3x3 are too far apart in height to blend.
	static bool NearCliff(const CellGrid& grid)
	{
		for (int y = -1; y <= 1; ++y)
		{
			for (int x = -1; x <= 1; ++x)
			{
				const Neighbour& cell = grid.At(x, y);

				if (!cell.Exists)
					continue;

				for (int axis = 0; axis < 2; ++axis)
				{
					const int otherX = x + (axis == 0 ? 1 : 0);
					const int otherY = y + (axis == 1 ? 1 : 0);

					if (otherX > 1 || otherY > 1)
						continue;

					const Neighbour& other = grid.At(otherX, otherY);

					if (other.Exists && std::abs(other.Level - cell.Level) > MaxLevelDifference)
						return true;
				}
			}
		}

		return false;
	}

	// Whether every usable cell within `radius` has the centre's palette and shade.
	static bool Uniform(const CellGrid& grid, int radius)
	{
		const Sample& centre = grid.At(0, 0).Value;

		for (int y = -radius; y <= radius; ++y)
		{
			for (int x = -radius; x <= radius; ++x)
			{
				const Neighbour& cell = grid.At(x, y);

				if (cell.Valid && cell.Value != centre)
					return false;
			}
		}

		return true;
	}

	// The in-plane quad of cell (x, y) for one diamond quadrant (bit 0 = X+1 side, bit 1 = Y+1 side).
	// Slots: the cell, its X neighbour, its Y neighbour, the diagonal one. A neighbour that can't be
	// blended with is replaced by the cell beside it on this side, so its share isn't lost to the cell.
	static Quad PlaneQuad(const CellGrid& grid, int x, int y, int quadrant)
	{
		const int sx = (quadrant & 1) ? 1 : -1;
		const int sy = (quadrant & 2) ? 1 : -1;
		const Neighbour& cell = grid.At(x, y);
		const Neighbour& nx = grid.At(x + sx, y);
		const Neighbour& ny = grid.At(x, y + sy);
		const Neighbour& nxy = grid.At(x + sx, y + sy);
		const bool planeX = CellGrid::Plane(nx, cell);
		const bool planeY = CellGrid::Plane(ny, cell);

		Quad quad;
		quad.Slot[0] = cell.Value;
		quad.Slot[1] = planeX ? nx.Value : cell.Value;
		quad.Slot[2] = planeY ? ny.Value : cell.Value;
		quad.Slot[3] = cell.Value;
		quad.Split = false;

		if (CellGrid::Plane(nxy, cell))
			quad.Slot[3] = nxy.Value;
		else if (planeX && planeY)
			quad.Split = true;
		else if (planeX)
			quad.Slot[3] = nx.Value;
		else if (planeY)
			quad.Slot[3] = ny.Value;

		quad.Uniform = quad.Slot[1] == quad.Slot[0] && quad.Slot[2] == quad.Slot[0]
			&& (quad.Split || quad.Slot[3] == quad.Slot[0]);
		return quad;
	}

	// Mirrors the vanilla painter's walk through tile variations: wrap the variation into
	// the variation count, then follow the chain of variation types that many steps.
	static IsometricTileTypeClass* ResolveVariation(IsometricTileTypeClass* pType, int variation)
	{
		const int count = pType->unk_2F0;

		if (variation <= 0 || count <= 0)
			return pType;

		if (count - 1 < variation)
			variation %= count;

		auto pNext = pType;

		for (; variation > 0; --variation)
		{
			pNext = reinterpret_cast<IsometricTileTypeClass*>(pNext->unk_2BC);

			if (!pNext)
				return pType;
		}

		return pNext;
	}

	class Painter
	{
	public:
		const WORD* Table;
		DWORD SpreadMask;
		DWORD Bias;
		Quad Plane[4]; // per diamond quadrant

		void Build(const CellGrid& grid)
		{
			for (int quadrant = 0; quadrant < 4; ++quadrant)
				this->Plane[quadrant] = PlaneQuad(grid, 0, 0, quadrant);
		}

		// Weights of the diamond pixel straight below or above (pixel x/y relative to the
		// top-left of the diamond). Pixels of the extra image use these away from cliffs.
		const Quad& NearestDiamondWeights(int pixelX, int pixelY, BYTE* weights) const
		{
			const int x = std::clamp(pixelX, 0, TileWidth - 1);
			const int y = std::clamp(pixelY, ColumnTop[x], ColumnBottom[x]);
			const PixelWeights& diamond = DiamondWeights[y][x];
			std::copy_n(diamond.W, 4, weights);
			return this->Plane[diamond.Quadrant];
		}

		inline WORD Shade(const Sample& sample, int alpha, BYTE index) const
		{
			return sample.Palette[this->Table[sample.Row + alpha] | index];
		}

		inline WORD Color(const Quad& quad, const BYTE* weights, WORD alphaValue, BYTE index) const
		{
			const int alpha = std::min<int>(alphaValue, 0xFF);

			if (quad.Uniform)
				return this->Shade(quad.Slot[0], alpha, index);

			const DWORD mask = this->SpreadMask;
			DWORD sum =
				Spread(this->Shade(quad.Slot[0], alpha, index), mask) * weights[0] + this->Bias;

			if (quad.Split)
			{
				const int half = weights[3] >> 1;
				sum += Spread(this->Shade(quad.Slot[1], alpha, index), mask) * (weights[1] + half);
				sum += Spread(this->Shade(quad.Slot[2], alpha, index), mask) * (weights[2] + weights[3] - half);
			}
			else
			{
				sum += Spread(this->Shade(quad.Slot[1], alpha, index), mask) * weights[1];
				sum += Spread(this->Shade(quad.Slot[2], alpha, index), mask) * weights[2];
				sum += Spread(this->Shade(quad.Slot[3], alpha, index), mask) * weights[3];
			}

			const DWORD packed = (sum >> 5) & mask;
			return static_cast<WORD>(packed | (packed >> 16));
		}

		// Blends two colours, `weight` 32ths of the way from the first to the second.
		inline WORD Mix(WORD first, WORD second, int weight) const
		{
			if (weight <= 0)
				return first;

			if (weight >= 32)
				return second;

			const DWORD mask = this->SpreadMask;
			const DWORD sum = Spread(first, mask) * (32 - weight) + Spread(second, mask) * weight + this->Bias;
			const DWORD packed = (sum >> 5) & mask;
			return static_cast<WORD>(packed | (packed >> 16));
		}
	};

	// Lighting for tiles next to a cliff.
	//
	// Cliff art is often drawn by a tile that doesn't border the face it shows, and the TMP
	// depth data puts such art on the drawing tile's own edges, so neither tells which wall a
	// pixel belongs to. Instead each pixel follows its view ray through the cell heights
	// around the tile (cells as flat tops at their level, walls where neighbours differ by
	// more than MaxLevelDifference) to the first surface it meets, a cell top or a wall. That
	// depends only on the screen position and the map, so every tile drawing part of the same
	// face agrees on it.
	//
	// Map offsets are in 120ths of a cell (see MapOffset). Raising a point one height level
	// moves it 15 px up the screen, the same as moving it half a cell along both X and Y, so
	// heights are kept in the same units: 60 per level.
	class CliffShader
	{
	public:
		void Init(const CellGrid& grid, const Painter& painter)
		{
			this->Grid = &grid;
			this->Owner = &painter;
			this->TopLevel = 0;

			const int centreLevel = grid.At(0, 0).Level;

			for (int y = -ScanRadius; y <= ScanRadius; ++y)
			{
				for (int x = -ScanRadius; x <= ScanRadius; ++x)
				{
					const Neighbour& cell = grid.At(x, y);
					ScanCell& scan = this->Scan[y + ScanRadius][x + ScanRadius];
					scan.Level = (cell.Level - centreLevel) * LevelUnits;
					scan.Usable = cell.Valid;
					scan.TopQuadsBuilt = 0;
					scan.WallQuadsBuilt = 0;

					if (cell.Valid)
						this->TopLevel = std::max(this->TopLevel, scan.Level);
				}
			}

			for (int i = 0; i < PixelRange; ++i)
			{
				this->Runs[i].Key = -1;
				this->RowOf[i] = INT_MIN;
			}
		}

		WORD Diamond(int pixelX, int pixelY, WORD alpha, BYTE index)
		{
			const Hit hit = this->FieldAt(pixelX, pixelY);

			if (hit.Kind == HitKind::Wall)
				return this->WallColor(pixelX, pixelY, hit, alpha, index);

			const PixelWeights& weights = DiamondWeights[pixelY][pixelX];
			return this->Owner->Color(this->Owner->Plane[weights.Quadrant], weights.W, alpha, index);
		}

		// pRow is the pixel's row of the extra image, `column` its position in it.
		WORD Extra(int pixelX, int pixelY, WORD alpha, BYTE index, const BYTE* pRow, int column, int width)
		{
			const Painter& painter = *this->Owner;
			const Hit hit = this->FieldAt(pixelX, pixelY);

			if (hit.Kind == HitKind::Wall)
				return this->WallColor(pixelX, pixelY, hit, alpha, index);

			BYTE weights[4];
			const int x = std::clamp(pixelX, 0, TileWidth - 1);
			const int distance = pixelY < ColumnTop[x] ? ColumnTop[x] - pixelY : pixelY - ColumnBottom[x];

			// Detail standing on the tile's own ground, such as rocks along a plateau's rim.
			if (distance <= ShortPixels)
				return painter.Color(painter.NearestDiamondWeights(pixelX, pixelY, weights), weights, alpha, index);

			const WORD own = hit.Kind == HitKind::Top
				? this->TopColor(pixelX, pixelY, hit, alpha, index)
				: painter.Color(painter.NearestDiamondWeights(pixelX, pixelY, weights), weights, alpha, index);

			// Cliff art often reaches past the side of the cliff, over ground that is lower
			// in the map. Fade it from the nearest raised surface in the same row to its own
			// lighting, finishing where this tile's art ends.
			const int ownLevel = hit.Kind == HitKind::Top ? this->LevelOf(hit.X, hit.Y) : INT_MAX;

			for (int offset = 1; offset <= FadePixels; ++offset)
			{
				for (int direction = -1; direction <= 1; direction += 2)
				{
					const int nearX = pixelX + direction * offset;
					const Hit nearHit = this->FieldAt(nearX, pixelY);
					const bool raised = nearHit.Kind == HitKind::Wall
						|| (nearHit.Kind == HitKind::Top && ownLevel != INT_MAX
							&& this->LevelOf(nearHit.X, nearHit.Y) - ownLevel > MaxLevelDifference * LevelUnits);

					if (!raised)
						continue;

					const WORD raisedColor = nearHit.Kind == HitKind::Wall
						? this->WallColor(nearX, pixelY, nearHit, alpha, index)
						: this->TopColor(nearX, pixelY, nearHit, alpha, index);

					int rest = 0;

					for (int c = column - direction; rest < FadePixels && c >= 0 && c < width && pRow[c]; c -= direction)
						++rest;

					const int span = std::min(FadePixels, offset + rest + 1);
					return painter.Mix(raisedColor, own, (offset * 32 + span / 2) / span);
				}
			}

			return own;
		}

	private:
		static constexpr int ScanRadius = 2;   // cells around the tile the lookup can land on
		static constexpr int CellUnits = 120;
		static constexpr int LevelUnits = 60;
		static constexpr int ShortPixels = 8;  // extra pixels this close to the diamond belong to the tile's own ground
		static constexpr int FadePixels = 16;  // longest fade of cliff art past the side of a cliff
		static constexpr int RunStep = 4;      // pixels per step when measuring a wall piece
		static constexpr int MaxRun = 256;
		static constexpr int PixelRange = 256; // per-column caches, for pixel x from -128 to 127

		enum class HitKind : BYTE
		{
			None,
			Top,
			Wall,
		};

		// Where a view ray first meets the terrain.
		struct Hit
		{
			HitKind Kind = HitKind::None;
			signed char X = 0; // the cell; for a wall, the higher cell behind it
			signed char Y = 0;
			BYTE Axis = 0;     // wall: the lower cell is at X+1 (0) or Y+1 (1)
			int Height = 0;    // in LevelUnits above the drawn tile

			bool SameWall(const Hit& other) const
			{
				return other.Kind == HitKind::Wall && this->Kind == HitKind::Wall
					&& other.X == this->X && other.Y == this->Y && other.Axis == this->Axis;
			}
		};

		struct Light
		{
			const Quad* Q;
			BYTE W[4];
		};

		// The visible piece of one wall in one screen column.
		struct Run
		{
			int Key;
			int Top;
			int Bottom;
			bool Interpolate; // false when what's above or below can't be resolved
			Light Above;
			Light Below;
		};

		struct ScanCell
		{
			int Level; // LevelUnits above the drawn tile
			bool Usable;
			BYTE TopQuadsBuilt;  // bit per quadrant
			BYTE WallQuadsBuilt; // bit per axis * 2 + side
			Quad TopQuads[4];
			Quad WallQuads[4];
		};

		const CellGrid* Grid;
		const Painter* Owner;
		int TopLevel;
		ScanCell Scan[2 * ScanRadius + 1][2 * ScanRadius + 1];
		Run Runs[PixelRange];
		Hit RowHits[PixelRange];
		int RowOf[PixelRange];

		static Hit MakeHit(HitKind kind, int x, int y, int axis, int height)
		{
			Hit hit;
			hit.Kind = kind;
			hit.X = static_cast<signed char>(x);
			hit.Y = static_cast<signed char>(y);
			hit.Axis = static_cast<BYTE>(axis);
			hit.Height = height;
			return hit;
		}

		static int FloorDiv(int value, int divisor)
		{
			return value >= 0 ? value / divisor : -((divisor - 1 - value) / divisor);
		}

		// The cell a map offset is in, taking a boundary as part of the cell below it.
		static int CellIndex(int offset)
		{
			return FloorDiv(offset + CellUnits / 2 - 1, CellUnits);
		}

		ScanCell& ScanAt(int x, int y)
		{
			return this->Scan[y + ScanRadius][x + ScanRadius];
		}

		int LevelOf(int x, int y) const
		{
			return this->Scan[y + ScanRadius][x + ScanRadius].Level;
		}

		// Follows the view ray of map offset (s, t) down from above the highest cell. The ray
		// passes over (s + h, t + h) at height h. Cells are visited front to back until the ray
		// is below a cell's top: if that happens where it enters the cell, it hit the wall
		// toward the cell it came from (or a small step, lit as that cell's edge).
		Hit March(int s, int t) const
		{
			constexpr int ScanEdge = CellUnits * ScanRadius + CellUnits / 2;

			int height = std::min(ScanEdge - s, ScanEdge - t);
			bool capped = false;

			if (this->TopLevel < height)
			{
				height = this->TopLevel;
				capped = true;
			}

			int x = CellIndex(s + height);
			int y = CellIndex(t + height);
			int fromLevel = 0;
			int fromAxis = -1;

			while (x >= -ScanRadius && y >= -ScanRadius && x <= ScanRadius && y <= ScanRadius)
			{
				const ScanCell& cell = this->Scan[y + ScanRadius][x + ScanRadius];

				if (!cell.Usable)
					break;

				if (cell.Level >= height)
				{
					if (fromAxis < 0)
						return capped ? MakeHit(HitKind::Top, x, y, 0, height) : Hit {};

					if (cell.Level - fromLevel > MaxLevelDifference * LevelUnits)
						return MakeHit(HitKind::Wall, x, y, fromAxis, height);

					return MakeHit(HitKind::Top, x + (fromAxis == 0), y + (fromAxis == 1), 0, height);
				}

				const int backX = CellUnits * x - CellUnits / 2 - s;
				const int backY = CellUnits * y - CellUnits / 2 - t;
				const int back = std::max(backX, backY);

				if (cell.Level >= back)
					return MakeHit(HitKind::Top, x, y, 0, cell.Level);

				fromLevel = cell.Level;

				if (backX >= backY)
				{
					fromAxis = 0;
					--x;
				}
				else
				{
					fromAxis = 1;
					--y;
				}

				height = back;
			}

			return Hit {};
		}

		Hit Trace(int pixelX, int pixelY) const
		{
			int s, t;
			MapOffset(pixelX, pixelY, s, t);
			return this->March(s, t);
		}

		// Traces with a cache of the last row traced in each column.
		Hit FieldAt(int pixelX, int pixelY)
		{
			const int slot = pixelX + PixelRange / 2;

			if (slot < 0 || slot >= PixelRange)
				return this->Trace(pixelX, pixelY);

			if (this->RowOf[slot] != pixelY)
			{
				this->RowOf[slot] = pixelY;
				this->RowHits[slot] = this->Trace(pixelX, pixelY);
			}

			return this->RowHits[slot];
		}

		const Quad& TopQuad(int x, int y, int quadrant)
		{
			ScanCell& cell = this->ScanAt(x, y);

			if (!(cell.TopQuadsBuilt & (1 << quadrant)))
			{
				cell.TopQuads[quadrant] = PlaneQuad(*this->Grid, x, y, quadrant);
				cell.TopQuadsBuilt |= 1 << quadrant;
			}

			return cell.TopQuads[quadrant];
		}

		// Slots: the lower cell, its neighbour along the wall, the higher cell, its neighbour
		// along the wall. Along neighbours that can't be blended with are replaced by the cell.
		const Quad& WallQuad(int x, int y, int axis, int side)
		{
			ScanCell& cell = this->ScanAt(x, y);
			const int bit = 1 << (axis * 2 + side);
			Quad& quad = cell.WallQuads[axis * 2 + side];

			if (!(cell.WallQuadsBuilt & bit))
			{
				const int step = side ? 1 : -1;
				const int alongX = axis == 0 ? 0 : step;
				const int alongY = axis == 0 ? step : 0;
				const int lowX = x + (axis == 0);
				const int lowY = y + (axis == 1);
				const Neighbour& high = this->Grid->At(x, y);
				const Neighbour& low = this->Grid->At(lowX, lowY);
				const Neighbour& highAlong = this->Grid->At(x + alongX, y + alongY);
				const Neighbour& lowAlong = this->Grid->At(lowX + alongX, lowY + alongY);

				quad.Slot[0] = low.Value;
				quad.Slot[1] = CellGrid::Plane(lowAlong, low) ? lowAlong.Value : low.Value;
				quad.Slot[2] = high.Value;
				quad.Slot[3] = CellGrid::Plane(highAlong, high) ? highAlong.Value : high.Value;
				quad.Split = false;
				quad.Uniform = quad.Slot[1] == quad.Slot[0] && quad.Slot[2] == quad.Slot[0] && quad.Slot[3] == quad.Slot[0];
				cell.WallQuadsBuilt |= bit;
			}

			return quad;
		}

		// The lighting where a ray hit: a cell top lit like that cell's own ground at that
		// point, or a wall faded from the lower cell at its foot to the higher one at its top.
		Light LightAt(int pixelX, int pixelY, const Hit& hit)
		{
			int s, t;
			MapOffset(pixelX, pixelY, s, t);
			Light light;

			if (hit.Kind == HitKind::Top)
			{
				const int offsetX = s + hit.Height - CellUnits * hit.X;
				const int offsetY = t + hit.Height - CellUnits * hit.Y;
				light.Q = &this->TopQuad(hit.X, hit.Y, (offsetX > 0 ? 1 : 0) | (offsetY > 0 ? 2 : 0));
				Bilinear(Toward(offsetX), Toward(offsetY), light.W);
			}
			else
			{
				const int along = hit.Axis == 0
					? t + hit.Height - CellUnits * hit.Y
					: s + hit.Height - CellUnits * hit.X;
				const int foot = this->LevelOf(hit.X + (hit.Axis == 0), hit.Y + (hit.Axis == 1));
				const int span = this->LevelOf(hit.X, hit.Y) - foot;
				const int up = std::clamp((32 * (hit.Height - foot) + span / 2) / span, 0, 32);
				light.Q = &this->WallQuad(hit.X, hit.Y, hit.Axis, along > 0 ? 1 : 0);
				Bilinear(Toward(along), up, light.W);
			}

			return light;
		}

		WORD Shade(const Light& light, WORD alpha, BYTE index) const
		{
			return this->Owner->Color(*light.Q, light.W, alpha, index);
		}

		WORD TopColor(int pixelX, int pixelY, const Hit& hit, WORD alpha, BYTE index)
		{
			return this->Shade(this->LightAt(pixelX, pixelY, hit), alpha, index);
		}

		// How far a wall piece continues from pixelY in one direction (-1 up, +1 down).
		int RunEnd(int pixelX, int pixelY, const Hit& hit, int direction) const
		{
			int end = pixelY;

			// Big steps first, then single pixels.
			while (std::abs(end - pixelY) < MaxRun && hit.SameWall(this->Trace(pixelX, end + direction * RunStep)))
				end += direction * RunStep;

			while (std::abs(end - pixelY) < MaxRun && hit.SameWall(this->Trace(pixelX, end + direction)))
				end += direction;

			return end;
		}

		// Seen side-on, a cliff is drawn as short pieces of wall stacked between plateau tiles.
		// Lit by height, each piece would end in a step where the next plateau tile cuts it off,
		// so a wall piece is interpolated between the surfaces just above and below it in its
		// screen column instead. On a wall seen whole that is the same as lighting it by height.
		WORD WallColor(int pixelX, int pixelY, const Hit& hit, WORD alpha, BYTE index)
		{
			const int key = (hit.Y + ScanRadius) * 10 + (hit.X + ScanRadius) * 2 + hit.Axis;
			const int slot = pixelX + PixelRange / 2;
			Run scratch;
			Run& run = slot >= 0 && slot < PixelRange ? this->Runs[slot] : scratch;

			if (&run == &scratch || run.Key != key || pixelY < run.Top || pixelY > run.Bottom)
			{
				run.Key = key;
				run.Top = this->RunEnd(pixelX, pixelY, hit, -1);
				run.Bottom = this->RunEnd(pixelX, pixelY, hit, 1);

				const Hit above = this->Trace(pixelX, run.Top - 1);
				const Hit below = this->Trace(pixelX, run.Bottom + 1);
				run.Interpolate = above.Kind != HitKind::None && below.Kind != HitKind::None;

				if (run.Interpolate)
				{
					run.Above = this->LightAt(pixelX, run.Top - 1, above);
					run.Below = this->LightAt(pixelX, run.Bottom + 1, below);
				}
			}

			if (!run.Interpolate)
				return this->Shade(this->LightAt(pixelX, pixelY, hit), alpha, index);

			const int span = run.Bottom - run.Top + 2;
			const int weight = ((pixelY - run.Top + 1) * 32 + span / 2) / span;
			return this->Owner->Mix(this->Shade(run.Above, alpha, index), this->Shade(run.Below, alpha, index), weight);
		}
	};

	template <typename TBuffer>
	static inline WORD* Advance(WORD* ptr, int count, TBuffer* pBuffer)
	{
		ptr += count;
		pBuffer->AdjustPointer(ptr);
		return ptr;
	}

	static void DrawDiamond(const Painter& painter, CliffShader* pCliff, const TmpBlock* pBlock, DSurface* pSurface,
		int x, int y, const RectangleStruct& bounds, bool useZ, WORD zBase)
	{
		const int columnMin = std::max(0, bounds.X - x);
		const int columnMax = std::min(TileWidth, bounds.X + bounds.Width - x);
		const int rowMin = std::max(0, bounds.Y - y);
		const int rowMax = std::min(TileRows, bounds.Y + bounds.Height - y);

		if (columnMin >= columnMax || rowMin >= rowMax)
			return;

		auto const pZBuffer = ZBuffer::Instance;
		auto const pABuffer = ABuffer::Instance;
		const int viewY = DSurface::ViewBounds.Y;
		const int left = x + columnMin;
		const int top = y + rowMin;

		auto pAlphaRow = static_cast<WORD*>(pABuffer->GetBuffer(left, top - viewY));
		auto pZRow = useZ ? static_cast<WORD*>(pZBuffer->GetBuffer(left, top - viewY)) : nullptr;

		if (!pAlphaRow || (useZ && !pZRow))
			return;

		auto const pDestination = static_cast<BYTE*>(pSurface->Lock(left, top));

		if (!pDestination)
			return;

		const int pitch = pSurface->GetPitch();
		const BYTE* const pImage = pBlock->Data(sizeof(TmpBlock));
		const BYTE* const pDepth = pBlock->Data(pBlock->ZOffset);

		for (int row = rowMin; row < rowMax; ++row)
		{
			const int start = std::max(RowStart[row], columnMin);
			const int end = std::min(RowStart[row] + RowWidth[row], columnMax);

			if (start < end)
			{
				const int source = RowOffset[row] + start - RowStart[row];
				const BYTE* pSource = pImage + source;
				const BYTE* pSourceZ = pDepth + source;
				const PixelWeights* pWeights = &DiamondWeights[row][start];
				auto pPixel = reinterpret_cast<WORD*>(pDestination + (row - rowMin) * pitch) + (start - columnMin);
				auto pAlpha = Advance(pAlphaRow, start - columnMin, pABuffer);
				auto pZ = useZ ? Advance(pZRow, start - columnMin, pZBuffer) : nullptr;

				for (int column = start; column < end; ++column)
				{
					bool draw = true;

					if (useZ)
					{
						const WORD z = static_cast<WORD>(*pSourceZ + zBase);
						draw = z <= *pZ;

						if (draw)
							*pZ = z;

						pZ = Advance(pZ, 1, pZBuffer);
					}

					if (draw)
					{
						*pPixel = pCliff
							? pCliff->Diamond(column, row, *pAlpha, *pSource)
							: painter.Color(painter.Plane[pWeights->Quadrant], pWeights->W, *pAlpha, *pSource);
					}

					++pPixel;
					++pSource;
					++pSourceZ;
					++pWeights;
					pAlpha = Advance(pAlpha, 1, pABuffer);
				}
			}

			pAlphaRow = Advance(pAlphaRow, pABuffer->Width, pABuffer);

			if (useZ)
				pZRow = Advance(pZRow, pZBuffer->Width, pZBuffer);
		}

		pSurface->Unlock();
	}

	static void DrawExtra(const Painter& painter, CliffShader* pCliff, const TmpBlock* pBlock, DSurface* pSurface,
		int x, int y, const RectangleStruct& bounds, bool useZ, WORD zBase)
	{
		const int extraX = x + pBlock->ExtraX - pBlock->X;
		const int extraY = y + pBlock->ExtraY - pBlock->Y;
		const int width = pBlock->ExtraWidth;
		const int height = pBlock->ExtraHeight;

		const int columnMin = std::max(0, bounds.X - extraX);
		const int columnMax = std::min(width, bounds.X + bounds.Width - extraX);
		const int rowMin = std::max(0, bounds.Y - extraY);
		const int rowMax = std::min(height, bounds.Y + bounds.Height - extraY);

		if (columnMin >= columnMax || rowMin >= rowMax)
			return;

		auto const pZBuffer = ZBuffer::Instance;
		auto const pABuffer = ABuffer::Instance;
		const int viewY = DSurface::ViewBounds.Y;
		const int left = extraX + columnMin;
		const int top = extraY + rowMin;

		auto pAlphaRow = static_cast<WORD*>(pABuffer->GetBuffer(left, top - viewY));
		auto pZRow = useZ ? static_cast<WORD*>(pZBuffer->GetBuffer(left, top - viewY)) : nullptr;

		if (!pAlphaRow || (useZ && !pZRow))
			return;

		auto const pDestination = static_cast<BYTE*>(pSurface->Lock(left, top));

		if (!pDestination)
			return;

		const int pitch = pSurface->GetPitch();
		const BYTE* const pImage = pBlock->Data(pBlock->ExtraOffset);
		const BYTE* const pDepth = pBlock->Data(pBlock->ExtraZOffset);
		BYTE weights[4];

		for (int row = rowMin; row < rowMax; ++row)
		{
			const BYTE* const pImageRow = pImage + row * width;
			const BYTE* pSource = pImageRow + columnMin;
			const BYTE* pSourceZ = pDepth + row * width + columnMin;
			auto pPixel = reinterpret_cast<WORD*>(pDestination + (row - rowMin) * pitch);
			auto pAlpha = pAlphaRow;
			auto pZ = pZRow;
			const int pixelY = extraY - y + row;

			for (int column = columnMin; column < columnMax; ++column)
			{
				if (const BYTE index = *pSource)
				{
					const WORD z = useZ ? static_cast<WORD>(*pSourceZ + zBase) : 0;

					if (!useZ || z <= *pZ)
					{
						const int pixelX = extraX - x + column;

						if (pCliff)
						{
							*pPixel = pCliff->Extra(pixelX, pixelY, *pAlpha, index, pImageRow, column, width);
						}
						else
						{
							const Quad& quad = painter.NearestDiamondWeights(pixelX, pixelY, weights);
							*pPixel = painter.Color(quad, weights, *pAlpha, index);
						}

						if (useZ)
							*pZ = z;
					}
				}

				++pPixel;
				++pSource;
				++pSourceZ;
				pAlpha = Advance(pAlpha, 1, pABuffer);

				if (useZ)
					pZ = Advance(pZ, 1, pZBuffer);
			}

			pAlphaRow = Advance(pAlphaRow, pABuffer->Width, pABuffer);

			if (useZ)
				pZRow = Advance(pZRow, pZBuffer->Width, pZBuffer);
		}

		pSurface->Unlock();
	}

	// Returns false when the vanilla painter should draw the tile instead. That decision is
	// always made before anything is drawn.
	static bool Draw(CellClass* pCell, IsometricTileTypeClass* pType, int variation, int subTile,
		int x, int y, const RectangleStruct& bounds)
	{
		// The vanilla painter has an alternative path behind this flag; the game never sets it.
		if (*reinterpret_cast<const bool*>(0xABC55C))
			return false;

		auto const pSurface = DSurface::Temp;
		auto const pConvert = pCell->LightConvert;

		if (!pSurface || !pConvert || !ZBuffer::Instance || !ABuffer::Instance)
			return false;

		PixelFormat format;

		if (pSurface->GetBytesPerPixel() != 2 || !GetPixelFormat(format))
			return false;

		pType = ResolveVariation(pType, variation);
		auto const pImage = reinterpret_cast<const TmpFile*>(pType->GetImage());

		if (!pImage || pImage->BlockWidth != TileWidth || pImage->BlockHeight != TileHeight)
			return false;

		const int blockCount = pImage->BlocksX * pImage->BlocksY;

		if (blockCount <= 0 || subTile < 0)
			return false;

		const TmpBlock* const pBlock = pImage->Blocks[subTile % blockCount];

		if (!pBlock)
			return true; // vanilla draws nothing either

		// Gather the cell and its eight neighbours.
		const int shadeCount = pConvert->ShadeCount;
		CellGrid grid;
		Neighbour& centre = grid.At(0, 0);
		centre.Level = pCell->Level;
		centre.Exists = true;
		centre.Valid = GetSample(pCell, shadeCount, centre.Value);

		if (!centre.Valid)
			return false;

		Gather(grid, pCell, shadeCount, 1, 1);

		// Next to a cliff the lighting can read cells up to GridRadius away.
		const bool nearCliff = NearCliff(grid);

		if (nearCliff)
			Gather(grid, pCell, shadeCount, 2, GridRadius);

		// Nothing to blend: the vanilla result is the same.
		if (Uniform(grid, nearCliff ? GridRadius : 1))
			return false;

		auto const pRemap = AlphaLightingRemapClass::FindOrAllocate(shadeCount);

		if (!pRemap)
			return false;

		InitTables();

		Painter painter;
		painter.Table = &pRemap->Table[0][0];
		painter.SpreadMask = format.SpreadMask;
		painter.Bias = format.Bias;
		painter.Build(grid);

		// Drawing is single-threaded, and the shader is too big for the stack.
		static CliffShader cliffShader;
		CliffShader* pCliff = nullptr;

		if (nearCliff)
		{
			cliffShader.Init(grid, painter);
			pCliff = &cliffShader;
		}

		const bool useZ = (pBlock->Flags & FlagBits::HasZData) != 0;
		auto const pZBuffer = ZBuffer::Instance;
		const WORD zBase = static_cast<WORD>(pZBuffer->MaxValue + pZBuffer->Bounds.Y - y - TileHeight - LevelHeight * pCell->Level);

		DrawDiamond(painter, pCliff, pBlock, pSurface, x, y, bounds, useZ, zBase);

		if (pBlock->Flags & FlagBits::HasExtra)
			DrawExtra(painter, pCliff, pBlock, pSurface, x, y, bounds, useZ, zBase);

		AlphaLightingRemapClass::Release(pRemap);
		return true;
	}
}

// Replaces the tile painter call inside CellClass::DrawTile. At this point the tile has an image.
DEFINE_HOOK(0x48043E, CellClass_DrawTile_SmoothLighting, 0x6)
{
	enum { SkipVanillaPainter = 0x48049E };

	if (!Phobos::Config::SmoothGroundLighting)
		return 0;

	GET(CellClass*, pCell, ESI);
	GET(IsometricTileTypeClass*, pType, EDI);
	GET(RectangleStruct*, pBounds, EBP);
	GET(int, tileY, EBX); // location Y minus the cell's height
	GET_STACK(int, subTile, STACK_OFFSET(0x1C, -0xC));
	GET_STACK(int, tileX, STACK_OFFSET(0x1C, -0x8));
	GET_STACK(int, variation, STACK_OFFSET(0x1C, 0xC));

	const int y = tileY + DSurface::ViewBounds.Y;

	return SmoothGroundLighting::Draw(pCell, pType, variation, subTile, tileX, y, *pBounds) ? SkipVanillaPainter : 0;
}
