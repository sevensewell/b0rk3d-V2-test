// cinematicaim.cpp
// "Кінематографічний" aim assist для Story Mode: коли ви цілитесь (ПКМ) і стріляєте (ЛКМ),
// додатково летить куля з вашої позиції в обрану частину тіла найближчого до центру камери НПС.
//
// Стиль повторює autocockweapon.cpp / nospread.cpp (LoopedCommand + статична реєстрація).
// Кладіть поруч із ними (src/game/features/...). Якщо CMake не збирає файли автоматично
// (GLOB), додайте цей файл у CMakeLists.txt.
//
// Усі натіви й сигнатури звірені з вашим Natives.hpp, перебір педів - з deleteallpeds.cpp.
// Єдине, що лишається "на перевірку", - ID кісток RDR2 (див. Target нижче).

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

		// Вимагати пряму видимість до цілі. За замовчуванням вимкнено: куля й так зупиниться об стіну.
		constexpr bool kRequireLineOfSight = false;

		inline void Tick(const Target& t, int part)
		{
			auto& d = g_CinematicAimDebug;
			d.ticks++;
			d.part = part;

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
				return;

			// Постріл: щойно натиснули ЛКМ або гра повідомляє, що ped стріляє.
			d.trigger = PAD::IS_CONTROL_JUST_PRESSED(0, (Hash)NativeInputs::INPUT_ATTACK) || PED::IS_PED_SHOOTING(self);

			// Хеш поточної зброї
			Hash weapon = 0;
			WEAPON::GET_CURRENT_PED_WEAPON(self, &weapon, 0, 0, 0);
			d.weapon = weapon;

			// Камера: позиція й напрямок "вперед"
			auto cp = CAM::GET_GAMEPLAY_CAM_COORD();
			auto cr = CAM::GET_GAMEPLAY_CAM_ROT(2);
			V3 camPos = {cp.x, cp.y, cp.z};
			constexpr float kDeg2Rad = 0.0174532925f;
			float pitch = cr.x * kDeg2Rad;
			float yaw   = cr.z * kDeg2Rad;
			V3 camFwd   = {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};

			// Скидаємо лічильники діагностики й шукаємо ціль (щокадру, поки цілимось)
			d.pedsTotal = d.pedsSkipped = d.pedsDead = d.pedsNotHuman = d.pedsInRange = d.pedsInFov = d.pedsLos = 0;
			d.nearestAngle = -1.0f;

			bool found      = false;
			V3 bestPoint    = {0, 0, 0};
			float bestAngle = kFovDegrees;

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

				V3 point    = TargetPoint(h, t);
				V3 toTarget = Sub(point, camPos);
				float dist  = Len(toTarget);
				if (dist < 0.5f || dist > kMaxDistance)
					continue;
				d.pedsInRange++;

				float c     = std::fmax(-1.0f, std::fmin(1.0f, Dot(camFwd, Norm(toTarget))));
				float angle = std::acos(c) * 57.2957795f;
				if (d.nearestAngle < 0.0f || angle < d.nearestAngle)
					d.nearestAngle = angle;
				if (angle > kFovDegrees)
					continue;
				d.pedsInFov++;

				bool los = ENTITY::HAS_ENTITY_CLEAR_LOS_TO_ENTITY(self, h, 17);
				if (los)
					d.pedsLos++;
				if (kRequireLineOfSight && !los)
					continue;

				if (angle < bestAngle)
				{
					bestAngle = angle;
					bestPoint = point;
					found     = true;
				}
			}

			if (!d.trigger || !found || !weapon)
				return;

			ULONGLONG now = GetTickCount64();
			if (now - g_LastShot < kCooldownMs)
				return;

			// Звідки летить куля: приблизно рівень грудей/плеча гравця.
			auto o = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(self, 0.2f, 0.4f, 0.5f);

			// (x1,y1,z1, x2,y2,z2, damage, perfectAccuracy, weapon, ownerPed, isAudible, isInvisible, velocity, p13)
			MISC::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(o.x, o.y, o.z, bestPoint.x, bestPoint.y, bestPoint.z, kDamage, true, weapon, self, true, false, 500.0f, false);

			g_LastShot = now;
			d.shots++;
		}
	}

	// Налаштування частини тіла (окрема команда, показується в меню через IntCommandItem "cinaimpart")
	static IntCommand _CinematicAimPart{"cinaimpart", "Cinematic Aim Body Part", "1=Head 2=Neck 3=Chest 4=Torso 5=Right Arm 6=Left Arm 7=Right Leg 8=Left Leg", 1};

	// Одна функція з вибором частини тіла через IntCommand (як у OldDeadeye).
	//   1 = Голова (трохи під капелюхом)   5 = Права рука
	//   2 = Шия                            6 = Ліва рука
	//   3 = Груди                          7 = Права нога
	//   4 = Торс (низ корпусу)             8 = Ліва нога
	// Зсуви: {boneId, право, вперед, вгору} у метрах від центру педа. boneId = 0 -> використовується зсув.
	class CinematicAim : public LoopedCommand
	{
		using LoopedCommand::LoopedCommand;

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

			int part = std::clamp(_CinematicAimPart.GetState(), 1, 8);
			CinematicAimImpl::Tick(kParts[part - 1], part);
		}
	};

	static CinematicAim _CinematicAim{"cinematicaim", "Cinematic Aim", "Story Mode only: while aiming, your shots also hit the chosen body part of the NPC nearest to the crosshair"};
}
