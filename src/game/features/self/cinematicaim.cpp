// cinematicaim.cpp
// "Кінематографічний" aim assist для Story Mode.
//
// Поки ви тримаєте ПКМ і ваш приціл опиняється в радіусі (у градусах) навколо обраної частини тіла НПС,
// камера плавно підтягує приціл до цієї частини тіла.
// Додатково (за бажанням) можна ввімкнути "магічну кулю", яка летить у ту саму точку.
//
// Усі натіви звірені з вашим Natives.hpp. Те, що я не міг перевірити без гри, позначено [TUNE]:
// це знаки напрямків і коефіцієнти, їх, можливо, доведеться підкрутити після першого тесту.

#include "core/commands/LoopedCommand.hpp"
#include "core/commands/IntCommand.hpp"
#include "game/features/CinematicAimDebug.hpp"
#include "game/backend/Self.hpp"
#include "game/rdr/Enums.hpp"
#include "game/rdr/Natives.hpp"
#include "game/rdr/Pools.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <windows.h>

namespace YimMenu::Features
{
	// ---------- Налаштування, які видно в меню (через IntCommandItem) ----------
	static IntCommand _CinematicAimPart{"cinaimpart", "Cinematic Aim Body Part", "1=Head 2=Neck 3=Chest 4=Torso 5=Right Arm 6=Left Arm 7=Right Leg 8=Left Leg", 1};
	static IntCommand _CinematicAimMode{"cinaimmode", "Cinematic Aim Assist Mode", "0=Off 1=Camera pull 2=Mouse input pull", 1};
	static IntCommand _CinematicAimRadius{"cinaimradius", "Cinematic Aim Radius", "How close (degrees, 1-45) the crosshair must be to the target for the assist to start", 10};
	static IntCommand _CinematicAimStrength{"cinaimstrength", "Cinematic Aim Strength", "Pull strength per frame in percent (1-100). Higher = snappier", 20};
	static IntCommand _CinematicAimBullet{"cinaimbullet", "Cinematic Aim Magic Bullet", "0=Off 1=Extra bullet flies to the chosen body part when you shoot", 0};

	namespace CinematicAimImpl
	{
		constexpr float kMaxDistance    = 60.0f; // метри
		constexpr int kDamage           = 100;   // шкода магічної кулі
		constexpr ULONGLONG kCooldownMs = 150;   // захист від подвійного пострілу
		constexpr float kDeadzoneDeg    = 0.15f; // не смикаємо камеру, якщо вже майже влучили
		constexpr float kMaxStepDeg     = 3.0f;  // максимум повороту за один кадр
		constexpr float kInputGain      = 0.04f; // [TUNE] режим 2: одиниць введення на градус
		constexpr float kRad2Deg        = 57.2957795f;
		constexpr float kDeg2Rad        = 0.0174532925f;

		struct Settings
		{
			int mode;
			float radius;
			float strength;
			bool bullet;
		};

		struct V3
		{
			float x, y, z;
		};

		inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
		inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
		inline V3 Norm(V3 a)
		{
			float l = Len(a);
			return l > 1e-6f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 0};
		}
		inline float WrapDeg(float a)
		{
			while (a > 180.0f)
				a -= 360.0f;
			while (a < -180.0f)
				a += 360.0f;
			return a;
		}
		inline float Clamp(float v, float lo, float hi) { return std::fmax(lo, std::fmin(hi, v)); }

		// Частина тіла: або ID кістки RDR2, або запасний зсув від центру педа.
		// boneId = 0 -> використовується зсув (право, вперед, вгору) у метрах від центру педа.
		// Коли знайдете й перевірите ID кісток RDR2, впишіть їх у boneId для точності по скелету.
		struct Target
		{
			int boneId;
			float ox, oy, oz;
		};

		inline V3 TargetPoint(int ped, const Target& t)
		{
			if (t.boneId != 0)
			{
				auto c = PED::GET_PED_BONE_COORDS(ped, t.boneId, 0.0f, 0.0f, 0.0f);
				return {c.x, c.y, c.z};
			}
			auto c = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(ped, t.ox, t.oy, t.oz);
			return {c.x, c.y, c.z};
		}

		inline ULONGLONG g_LastShot = 0;
		inline int g_LockedPed      = 0; // ціль, за якою тримаємось, поки ви цілитесь

		inline void Tick(const Target& t, int part, const Settings& s)
		{
			auto& d = g_CinematicAimDebug;
			d.ticks++;
			d.part = part;
			d.mode = s.mode;
			d.assistActive = false;

			auto selfPed = Self::GetPed();
			d.selfValid  = static_cast<bool>(selfPed);
			if (!selfPed)
				return;

			// Тільки Story Mode: в онлайн-сесії функція нічого не робить.
			d.sessionActive = NETWORK::NETWORK_IS_SESSION_ACTIVE();
			if (d.sessionActive)
				return;

			int self     = selfPed.GetHandle();
			int playerId = Self::GetPlayer().GetId();

			// Прицілювання: кнопка, або прапорець гравця, або активна камера прицілу.
			d.aiming = PAD::IS_CONTROL_PRESSED(0, (Hash)NativeInputs::INPUT_AIM)
			    || PLAYER::IS_PLAYER_FREE_AIMING(playerId)
			    || CAM::IS_AIM_CAM_ACTIVE();
			if (!d.aiming)
			{
				g_LockedPed = 0;
				return;
			}

			d.trigger = PAD::IS_CONTROL_JUST_PRESSED(0, (Hash)NativeInputs::INPUT_ATTACK) || PED::IS_PED_SHOOTING(self);

			Hash weapon = 0;
			WEAPON::GET_CURRENT_PED_WEAPON(self, &weapon, 0, 0, 0);
			d.weapon = weapon;

			// Камера: позиція й кути (x = pitch, z = yaw, у градусах)
			auto cp = CAM::GET_GAMEPLAY_CAM_COORD();
			auto cr = CAM::GET_GAMEPLAY_CAM_ROT(2);
			V3 camPos = {cp.x, cp.y, cp.z};
			float pitch = cr.x * kDeg2Rad;
			float yaw   = cr.z * kDeg2Rad;
			V3 camFwd   = {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};

			d.pedsTotal = d.pedsSkipped = d.pedsDead = d.pedsNotHuman = d.pedsOffScreen = d.pedsInRange = d.pedsInRadius = 0;
			d.nearestAngle = -1.0f;

			bool found        = false;
			int bestHandle    = 0;
			V3 bestPoint      = {0, 0, 0};
			float bestScore   = 1e9f;

			for (auto ped : Pools::GetPeds())
			{
				d.pedsTotal++;
				if (!ped || ped.IsPlayer() || ped.GetHandle() == self)
				{
					d.pedsSkipped++;
					continue;
				}
				int h = ped.GetHandle();

				if (ENTITY::IS_ENTITY_DEAD(h))
				{
					d.pedsDead++;
					continue;
				}
				if (!PED::IS_PED_HUMAN(h))
				{
					d.pedsNotHuman++;
					continue;
				}
				if (!ENTITY::IS_ENTITY_ON_SCREEN(h))
				{
					d.pedsOffScreen++;
					continue;
				}

				V3 point    = TargetPoint(h, t);
				V3 toTarget = Sub(point, camPos);
				float dist  = Len(toTarget);
				if (dist < 0.5f || dist > kMaxDistance)
					continue;
				d.pedsInRange++;

				float angle = std::acos(Clamp(Dot(camFwd, Norm(toTarget)), -1.0f, 1.0f)) * kRad2Deg;
				if (d.nearestAngle < 0.0f || angle < d.nearestAngle)
					d.nearestAngle = angle;

				// Ціль, за якою вже тримаємось, має запас: не стрибаємо між НПС
				bool locked    = (h == g_LockedPed);
				float limit    = locked ? s.radius * 1.5f : s.radius;
				if (angle > limit)
					continue;
				d.pedsInRadius++;

				float score = locked ? angle * 0.5f : angle;
				if (score < bestScore)
				{
					bestScore  = score;
					bestHandle = h;
					bestPoint  = point;
					found      = true;
				}
			}

			g_LockedPed = found ? bestHandle : 0;
			if (!found)
				return;

			V3 to      = Sub(bestPoint, camPos);
			float flat = std::sqrt(to.x * to.x + to.y * to.y);

			// ---- Підтягування прицілу ----
			if (s.mode != 0)
			{
				float desiredYaw   = std::atan2(-to.x, to.y) * kRad2Deg; // forward = (-sin yaw, cos yaw)
				float desiredPitch = std::atan2(to.z, flat) * kRad2Deg;
				float dYaw         = WrapDeg(desiredYaw - cr.z);
				float dPitch       = desiredPitch - cr.x;
				d.dYaw             = dYaw;
				d.dPitch           = dPitch;

				float stepYaw   = (std::fabs(dYaw) < kDeadzoneDeg) ? 0.0f : Clamp(dYaw * s.strength, -kMaxStepDeg, kMaxStepDeg);
				float stepPitch = (std::fabs(dPitch) < kDeadzoneDeg) ? 0.0f : Clamp(dPitch * s.strength, -kMaxStepDeg, kMaxStepDeg);

				if (stepYaw != 0.0f || stepPitch != 0.0f)
				{
					d.assistActive = true;
					if (s.mode == 1)
					{
						// [TUNE] Якщо камера тікає в протилежний бік, змініть знак перед stepYaw/stepPitch.
						CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING() + stepYaw, 1.0f);
						CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(CAM::GET_GAMEPLAY_CAM_RELATIVE_PITCH() + stepPitch, 1.0f);
					}
					else
					{
						// [TUNE] Імітація руху миші: вправо/вниз = додатне значення, тому знаки мінусові.
						PAD::SET_CONTROL_VALUE_NEXT_FRAME(0, (Hash)NativeInputs::INPUT_LOOK_LR, -stepYaw * kInputGain);
						PAD::SET_CONTROL_VALUE_NEXT_FRAME(0, (Hash)NativeInputs::INPUT_LOOK_UD, -stepPitch * kInputGain);
					}
				}
			}

			// ---- Магічна куля (за бажанням) ----
			if (s.bullet && d.trigger && weapon)
			{
				ULONGLONG now = GetTickCount64();
				if (now - g_LastShot >= kCooldownMs)
				{
					auto o = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(self, 0.2f, 0.4f, 0.5f);
					// (x1,y1,z1, x2,y2,z2, damage, perfectAccuracy, weapon, ownerPed, isAudible, isInvisible, velocity, p13)
					MISC::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(o.x, o.y, o.z, bestPoint.x, bestPoint.y, bestPoint.z, kDamage, true, weapon, self, true, false, 500.0f, false);
					g_LastShot = now;
					d.shots++;
				}
			}
		}
	}

	// Одна функція (перемикач у меню) + налаштування вище.
	class CinematicAim : public LoopedCommand
	{
		using LoopedCommand::LoopedCommand;

		virtual void OnTick() override
		{
			// Зсуви: {boneId, право, вперед, вгору} у метрах від центру педа. boneId = 0 -> зсув.
			static const CinematicAimImpl::Target kParts[8] = {
			    {0, 0.00f, 0.0f, 0.58f},  // 1 голова (під капелюх)
			    {0, 0.00f, 0.0f, 0.47f},  // 2 шия
			    {0, 0.00f, 0.0f, 0.30f},  // 3 груди
			    {0, 0.00f, 0.0f, 0.05f},  // 4 торс
			    {0, 0.25f, 0.0f, 0.25f},  // 5 права рука
			    {0, -0.25f, 0.0f, 0.25f}, // 6 ліва рука
			    {0, 0.12f, 0.0f, -0.45f}, // 7 права нога
			    {0, -0.12f, 0.0f, -0.45f} // 8 ліва нога
			};

			int part = std::clamp(_CinematicAimPart.GetState(), 1, 8);

			CinematicAimImpl::Settings s;
			s.mode     = std::clamp(_CinematicAimMode.GetState(), 0, 2);
			s.radius   = static_cast<float>(std::clamp(_CinematicAimRadius.GetState(), 1, 45));
			s.strength = static_cast<float>(std::clamp(_CinematicAimStrength.GetState(), 1, 100)) / 100.0f;
			s.bullet   = _CinematicAimBullet.GetState() != 0;

			CinematicAimImpl::Tick(kParts[part - 1], part, s);
		}
	};

	static CinematicAim _CinematicAim{"cinematicaim", "Cinematic Aim", "Story Mode only: while you hold aim, the crosshair is gently pulled to the chosen body part of a nearby NPC"};
}
