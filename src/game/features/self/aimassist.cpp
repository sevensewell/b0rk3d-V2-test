#include "core/commands/LoopedCommand.hpp"
#include "game/backend/Self.hpp"
#include "game/backend/Players.hpp"
#include "game/rdr/Natives.hpp"
#include "game/rdr/Enums.hpp" // Для INPUT_AIM

namespace YimMenu::Features
{
    // ID костей для RDR2 (взяты из вашего Esp.hpp)
    static constexpr int BoneIds[] = {
        21030, // Head
        14283, // Neck
        14410, // Torso
        34606, // Left Hand (можно заменить на плечо/локоть при желании)
        22798, // Right Hand
        45454, // Left Foot
        33646  // Right Foot
    };

    class AimAssist : public LoopedCommand
    {
        using LoopedCommand::LoopedCommand;

        virtual void OnTick() override
        {
            if (!_AimAssistEnabled.GetState())
                return;

            // Работает только когда игрок целится (ПКМ / LT)
            if (!PAD::IS_CONTROL_PRESSED(0, (Hash)NativeInputs::INPUT_AIM))
                return;

            // Получаем педа игрока
            auto selfPed = Self::GetPed();
            if (!selfPed.IsValid())
                return;

            // --- Поиск цели ---
            // Вариант 1: использовать выбранного в меню Players игрока
            Ped targetPed = 0;
            if (auto selected = Players::GetSelected(); selected.IsValid() && selected.GetPed().IsValid())
            {
                targetPed = selected.GetPed().GetHandle();
            }
            else
            {
                // Вариант 2 (если никто не выбран): можно добавить поиск ближайшего педа через Pools::GetPeds()
                // Но для простоты пока оставим только выбранного игрока.
                return;
            }

            if (!ENTITY::DOES_ENTITY_EXIST(targetPed) || ENTITY::IS_ENTITY_DEAD(targetPed))
                return;

            // Не целимся в себя
            if (targetPed == selfPed.GetHandle())
                return;

            // --- Выбор кости ---
            int boneIndex = _AimAssistBone.GetState();
            if (boneIndex < 0 || boneIndex >= IM_ARRAYSIZE(BoneIds))
                boneIndex = 0; // fallback на голову

            int boneId = BoneIds[boneIndex];

            // Получаем мировые координаты выбранной кости цели
            Vector3 bonePos = PED::GET_PED_BONE_COORDS(targetPed, boneId, 0.0f, 0.0f, 0.0f);

            // Заставляем нашего педа целиться в эту точку.
            // TASK_AIM_GUN_AT_COORD(ped, x, y, z, duration, p5, p6)
            // duration = 100 мс (обновляем каждый тик), p5/p6 = false (не ждать завершения)
            TASK::TASK_AIM_GUN_AT_COORD(selfPed.GetHandle(), bonePos.x, bonePos.y, bonePos.z, 100, false, false);
        }
    };

    static AimAssist _AimAssist{"aimassist", "Aim Assist", "Automatically aims at selected bone of target"};
}
