#pragma once

#include <Windows.h>
#include <cstring>

// Shared arithmetic for predicting the player's next move. The games do not share input bit
// assignments or speed fields, so each game still has to say which fields and bits it uses.
namespace PlayerInterpolation {
	enum Direction {
		DirectionNone = 0,
		DirectionUp,
		DirectionDown,
		DirectionLeft,
		DirectionRight,
		DirectionUpLeft,
		DirectionUpRight,
		DirectionDownLeft,
		DirectionDownRight,
	};

	struct Controls {
		Direction direction;
		bool focus;
	};

	struct Step {
		float x;
		float y;
	};

	inline Direction StandardDirection(unsigned int input) {
		const bool up = (input & 0x10) != 0;
		const bool down = (input & 0x20) != 0;
		const bool left = (input & 0x40) != 0;
		const bool right = (input & 0x80) != 0;
		if (up) {
			if (left)
				return DirectionUpLeft;
			if (right)
				return DirectionUpRight;
			return DirectionUp;
		}
		if (down) {
			if (left)
				return DirectionDownLeft;
			if (right)
				return DirectionDownRight;
			return DirectionDown;
		}
		if (left)
			return DirectionLeft;
		if (right)
			return DirectionRight;
		return DirectionNone;
	}

	inline Direction StandardDirection(unsigned int input, unsigned int up, unsigned int down,
		unsigned int left, unsigned int right) {
		const bool up_down = (input & up) != 0;
		const bool down_down = (input & down) != 0;
		const bool left_down = (input & left) != 0;
		const bool right_down = (input & right) != 0;
		if (up_down) {
			if (left_down)
				return DirectionUpLeft;
			if (right_down)
				return DirectionUpRight;
			return DirectionUp;
		}
		if (down_down) {
			if (left_down)
				return DirectionDownLeft;
			if (right_down)
				return DirectionDownRight;
			return DirectionDown;
		}
		if (left_down)
			return DirectionLeft;
		if (right_down)
			return DirectionRight;
		return DirectionNone;
	}

	inline Controls StandardControls(unsigned int input, unsigned int focus_mask) {
		Controls controls = { StandardDirection(input), (input & focus_mask) != 0 };
		return controls;
	}

	inline Step FromSpeeds(Direction direction, bool focus, float orthogonal, float focus_orthogonal,
		float diagonal, float focus_diagonal) {
		const float straight = focus ? focus_orthogonal : orthogonal;
		const float corner = focus ? focus_diagonal : diagonal;
		Step step = { 0.0f, 0.0f };
		switch (direction) {
		case DirectionUp:
			step.y = -straight;
			break;
		case DirectionDown:
			step.y = straight;
			break;
		case DirectionLeft:
			step.x = -straight;
			break;
		case DirectionRight:
			step.x = straight;
			break;
		case DirectionUpLeft:
			step.x = step.y = -corner;
			break;
		case DirectionUpRight:
			step.x = corner;
			step.y = -corner;
			break;
		case DirectionDownLeft:
			step.x = -corner;
			step.y = corner;
			break;
		case DirectionDownRight:
			step.x = step.y = corner;
			break;
		default:
			break;
		}
		return step;
	}

	// th10 and later keep these as integer fixed-point speeds rather than floats.
	inline Step FromSpeeds(Direction direction, bool focus, int orthogonal, int focus_orthogonal,
		int diagonal, int focus_diagonal) {
		return FromSpeeds(direction, focus, (float)orthogonal, (float)focus_orthogonal,
			(float)diagonal, (float)focus_diagonal);
	}

	inline void ClampStep(float x, float y, float min_x, float max_x, float min_y, float max_y,
		Step* step) {
		if (step->x < 0.0f && x + step->x < min_x)
			step->x = min_x - x;
		else if (step->x > 0.0f && x + step->x > max_x)
			step->x = max_x - x;
		if (step->y < 0.0f && y + step->y < min_y)
			step->y = min_y - y;
		else if (step->y > 0.0f && y + step->y > max_y)
			step->y = max_y - y;
	}

	// Some games expose their complete keyboard/controller poller but no side-effect-free getter.
	// A poll is safe to reuse when its only remaining effects are the input state and the button
	// history stored beside it: snapshot that block, poll, then put it back before game code sees it.
	template <size_t Size, typename Poll>
	inline unsigned int PollWithoutState(Poll poll, void* state) {
		unsigned char saved[Size];
		memcpy(saved, state, Size);
		const unsigned int input = poll();
		memcpy(state, saved, Size);
		return input;
	}

	template <size_t Size, typename Poll, typename Arg>
	inline unsigned int PollWithoutState(Poll poll, Arg arg, void* state) {
		unsigned char saved[Size];
		memcpy(saved, state, Size);
		const unsigned int input = poll(arg);
		memcpy(state, saved, Size);
		return input;
	}
}
