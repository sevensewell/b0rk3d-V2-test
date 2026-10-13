#include "core/commands/LoopedCommand.hpp"
#include "core/commands/IntCommand.hpp"
#include "game/backend/Self.hpp"
#include "game/rdr/Enums.hpp"
#include "game/rdr/Natives.hpp"
#include "game/rdr/Pools.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <windows.h>

namespace YimMenu::Features
{
	namespace CinematicAimImpl
	{
		// Налаштування (поки константи; пізніше можна винести в меню)
		constexpr float kMaxDistance = 60.0f; // метри
		constexpr float kFovDegrees  = 25.0f; // конус навколо центру камери
		constexpr int kDamage        = 100;   // шкода магічної кулі
		constexpr ULONGLONG kCooldownMs = 150; // захист від подвійного пострілу, якщо ввімкнено кілька режимів

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

		// Частина тіла: або ID кістки RDR2, або запасний зсув від центру педа.
		// boneId = 0 -> використовується зсув (ox, oy, oz) у локальних координатах педа.
		// Зсуви працюють "з коробки" для людини, що стоїть. Для сидячих/мертвих/на коні будуть неточні.
		// Коли знайдете й перевірите ID кісток RDR2, впишіть їх у boneId - тоді точність піде по скелету.
		struct Target
		{
			int boneId;
			float ox, oy, oz;
		};

		// Список НПС з пулу педів (як у DeleteAllPeds). Живих гравців пропускаємо через IsPlayer().
		inline std::vector<int> GetPedHandles()
		{
			std::vector<int> out;
			for (auto ped : Pools::GetPeds())
			{
				if (!ped || ped.IsPlayer())
					continue;
				out.push_back(ped.GetHandle());
			}
			return out;
		}

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

		inline void Tick(const Target& t)
		{
			auto selfPed = Self::GetPed();
			if (!selfPed)
				return;

			// Тільки Story Mode: в онлайн-сесії функція нічого не робить.
			if (NETWORK::NETWORK_IS_SESSION_ACTIVE())
				return;

			// Працюємо лише коли ціляться і тільки в момент пострілу.
			if (!PAD::IS_CONTROL_PRESSED(0, (Hash)NativeInputs::INPUT_AIM))
				return;
			if (!PAD::IS_CONTROL_JUST_PRESSED(0, (Hash)NativeInputs::INPUT_ATTACK))
				return;

			ULONGLONG now = GetTickCount64();
			if (now - g_LastShot < kCooldownMs)
				return;

			int self = selfPed.GetHandle();

			// Камера: позиція й напрямок "вперед" з кутів Ейлера.
			auto cp = CAM::GET_GAMEPLAY_CAM_COORD();
			auto cr = CAM::GET_GAMEPLAY_CAM_ROT(2); // x = pitch, z = yaw (в градусах)
			V3 camPos = {cp.x, cp.y, cp.z};
			constexpr float kDeg2Rad = 0.0174532925f;
			float pitch = cr.x * kDeg2Rad;
			float yaw   = cr.z * kDeg2Rad;
			V3 camFwd = {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};

			int bestPed     = 0;
			V3 bestPoint    = {0, 0, 0};
			float bestAngle = kFovDegrees;

			for (int ped : GetPedHandles())
			{
				if (ped == self)
					continue;
				if (ENTITY::IS_ENTITY_DEAD(ped))
					continue;
				if (!PED::IS_PED_HUMAN(ped)) // ігноруємо тварин
					continue;

				V3 point    = TargetPoint(ped, t);
				V3 toTarget = Sub(point, camPos);
				float dist  = Len(toTarget);
				if (dist < 0.5f || dist > kMaxDistance)
					continue;

				float c = Dot(camFwd, Norm(toTarget));
				c       = std::fmax(-1.0f, std::fmin(1.0f, c));
				float angle = std::acos(c) * 57.2957795f;
				if (angle > bestAngle)
					continue;

				if (!ENTITY::HAS_ENTITY_CLEAR_LOS_TO_ENTITY(self, ped, 17))
					continue;

				bestAngle = angle;
				bestPed   = ped;
				bestPoint = point;
			}

			if (!bestPed)
				return;

			// Звідки летить куля: приблизно рівень грудей/плеча гравця.
			auto o = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(self, 0.2f, 0.4f, 0.5f);

			// Хеш поточної зброї.
			Hash weapon = 0;
			WEAPON::GET_CURRENT_PED_WEAPON(self, &weapon, 0, 0, 0);
			if (!weapon)
				return;

			// Сигнатура з вашого Natives.hpp:
			// (x1,y1,z1, x2,y2,z2, damage, perfectAccuracy, weapon, ownerPed, isAudible, isInvisible, velocity, p13)
			MISC::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(o.x, o.y, o.z, bestPoint.x, bestPoint.y, bestPoint.z, kDamage, true, weapon, self, true, false, 500.0f, false);

			g_LastShot = now;
		}
	}

	// Одна функція з вибором частини тіла через IntCommand (як у OldDeadeye).
	//   1 = Голова (трохи під капелюхом)   5 = Права рука
	//   2 = Шия                            6 = Ліва рука
	//   3 = Груди                          7 = Права нога
	//   4 = Торс (низ корпусу)             8 = Ліва нога
	// Зсуви: {boneId, право, вперед, вгору} у метрах від центру педа. boneId = 0 -> використовується зсув.
	class CinematicAim : public LoopedCommand
	{
		using LoopedCommand::LoopedCommand;

		IntCommand _BodyPart{
		    "cinaimpart",
		    "Cinematic Aim Body Part",
		    "1=Head 2=Neck 3=Chest 4=Torso 5=Right Arm 6=Left Arm 7=Right Leg 8=Left Leg",
		    1};

		virtual void OnTick() override
		{
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

			int part = std::clamp(_BodyPart.GetState(), 1, 8);
			CinematicAimImpl::Tick(kParts[part - 1]);
		}
	};

	static CinematicAim _CinematicAim{"cinematicaim", "Cinematic Aim", "Story Mode only: while aiming, your shots also hit the chosen body part of the NPC nearest to the crosshair"};
}
