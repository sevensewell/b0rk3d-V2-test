#pragma once
// Діагностика для Cinematic Aim. Кладіть поруч з Features.hpp (src/game/features/).
namespace YimMenu::Features
{
	struct CinematicAimDebug
	{
		bool selfValid     = false;
		bool aiming        = false;
		bool trigger       = false; // момент пострілу
		unsigned weapon    = 0;
		int pedsTotal      = 0;
		int pedsSkipped    = 0; // гравці / сам гравець
		int pedsDead       = 0;
		int pedsNotHuman   = 0;
		int pedsOffScreen  = 0;
		int pedsInRange    = 0;
		int pedsInRadius   = 0; // у радіусі навколо прицілу
		int playersTotal   = 0;
		int playersDead    = 0;
		int playersNotHuman = 0;
		int playersOffScreen = 0;
		int playersInRange = 0;
		int playersInRadius  = 0;
		float nearestAngle = -1.0f; // найменший кут до центру камери серед живих людей на екрані
		bool assistActive  = false; // зараз тягнемо приціл
		float dYaw         = 0.0f;
		float dPitch       = 0.0f;
		int mode           = 0;
		// Перевірка ID кістки на найближчому НПС на екрані
		int boneIdUsed     = 0;   // 0 = використовуються зсуви
		int boneIndex      = -2;  // результат GET_PED_BONE_INDEX (-1 = кістки з таким ID нема), -2 = не перевірялось
		float boneRelX     = 0.0f; // позиція кістки відносно центру педа (м)
		float boneRelY     = 0.0f;
		float boneRelZ     = 0.0f;
		bool boneRawZero   = false; // натив повернув (0,0,0)
		int part           = 0;
		int shots          = 0;
		unsigned long long ticks = 0;
	};

	inline CinematicAimDebug g_CinematicAimDebug;
}
