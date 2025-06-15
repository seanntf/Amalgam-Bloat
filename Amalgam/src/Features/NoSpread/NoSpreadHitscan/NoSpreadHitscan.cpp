#include "NoSpreadHitscan.h"

#include "../../Ticks/Ticks.h"
#include <regex>
#include <numeric>
#include <algorithm> 

MAKE_SIGNATURE(CTFWeaponBaseGun_GetWeaponSpread, "client.dll", "48 89 5C 24 ? 57 48 83 EC ? 4C 63 91", 0x0);

static Color_t BlendColors(const Color_t& a, const Color_t& b, float ratio) 
{
    Color_t result;
    result.r = static_cast<byte>(a.r * (1.0f - ratio) + b.r * ratio);
    result.g = static_cast<byte>(a.g * (1.0f - ratio) + b.g * ratio);
    result.b = static_cast<byte>(a.b * (1.0f - ratio) + b.b * ratio);
    result.a = static_cast<byte>(a.a * (1.0f - ratio) + b.a * ratio);
    return result;
}

void CNoSpreadHitscan::Reset()
{
	m_bWaitingForPlayerPerf = false;
	m_flServerTime = 0.f;
	m_vTimeDeltas.clear();
	m_dTimeDelta = 0.0;

	m_iSeed = 0;
	m_flMantissaStep = 0.f;

	m_bSynced = false;
}

bool CNoSpreadHitscan::ShouldRun(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, bool bCreateMove)
{
	if (G::PrimaryWeaponType != EWeaponType::HITSCAN
		|| (bCreateMove ? pWeapon->GetWeaponSpread() : S::CTFWeaponBaseGun_GetWeaponSpread.Call<float>(pWeapon)) <= 0.f)
		return false;

	return bCreateMove ? G::Attacking == 1 : true;
}

int CNoSpreadHitscan::GetSeed(CUserCmd* pCmd)
{
	double dFloatTime = SDK::PlatFloatTime() + m_dTimeDelta;
	float flTime = float(dFloatTime * 1000.0);
	return std::bit_cast<int32_t>(flTime) & 255;
}

float CNoSpreadHitscan::CalcMantissaStep(float flV)
{
	// Calculate the delta to the next representable value
	float nextValue = std::nextafter(flV, std::numeric_limits<float>::infinity());
	float mantissaStep = (nextValue - flV) * 1000;

	// Get the closest mantissa (next power of 2)
	return powf(2, ceilf(logf(mantissaStep) / logf(2)));
}

std::string CNoSpreadHitscan::GetFormat(int iServerTime)
{
	int iDays = iServerTime / 86400;
	int iHours = iServerTime / 3600 % 24;
	int iMinutes = iServerTime / 60 % 60;
	int iSeconds = iServerTime % 60;

	if (iDays)
		return std::format("{}d {}h", iDays, iHours);
	else if (iHours)
		return std::format("{}h {}m", iHours, iMinutes);
	else
		return std::format("{}m {}s", iMinutes, iSeconds);
}

void CNoSpreadHitscan::AskForPlayerPerf()
{
	if (!Vars::Aimbot::General::NoSpread.Value || !I::EngineClient->IsInGame())
		return Reset();

	if (G::Choking)
		return;

	static Timer tTimer = {};
	if (!m_bWaitingForPlayerPerf ? tTimer.Run(Vars::Aimbot::General::NoSpreadInterval.Value) : tTimer.Run(Vars::Aimbot::General::NoSpreadBackupInterval.Value))
	{
		I::ClientState->SendStringCmd("playerperf");
		m_bWaitingForPlayerPerf = true;
		m_dRequestTime = SDK::PlatFloatTime();
	}
}

bool CNoSpreadHitscan::ParsePlayerPerf(std::string sMsg)
{
	if (!Vars::Aimbot::General::NoSpread.Value)
		return false;

	std::smatch match; std::regex_match(sMsg, match, std::regex(R"((\d+.\d+)\s\d+\s\d+\s\d+.\d+\s\d+.\d+\svel\s\d+.\d+)"));

	if (match.size() == 2)
	{
		m_bWaitingForPlayerPerf = false;

		// credits to kgb for idea
		float flNewServerTime = std::stof(match[1].str());
		if (flNewServerTime < m_flServerTime)
			return true;

		auto pNetChan = I::EngineClient->GetNetChannelInfo();
		bool bLoopback = pNetChan && pNetChan->IsLoopback();

		m_flServerTime = flNewServerTime;

		if (bLoopback)
			m_dTimeDelta = 0.f;
		else
		{
			m_vTimeDeltas.push_back(m_flServerTime - m_dRequestTime + TICKS_TO_TIME(1));
			while (!m_vTimeDeltas.empty() && m_vTimeDeltas.size() > Vars::Aimbot::General::NoSpreadAverage.Value)
				m_vTimeDeltas.pop_front();
			m_dTimeDelta = std::reduce(m_vTimeDeltas.begin(), m_vTimeDeltas.end()) / m_vTimeDeltas.size();
		}
		m_dTimeDelta += TICKS_TO_TIME(Vars::Aimbot::General::NoSpreadOffset.Value);

		float flMantissaStep = CalcMantissaStep(m_flServerTime);
		m_bSynced = flMantissaStep >= 1.f || bLoopback;

		if (flMantissaStep > m_flMantissaStep && (m_bSynced || !m_flMantissaStep))
		{
			SDK::Output("Seed Prediction", m_bSynced ? std::format("Synced ({})", m_dTimeDelta).c_str() : "Not synced, step too low", Vars::Menu::Theme::Accent.Value);
			SDK::Output("Seed Prediction", std::format("Age {}; Step {}", GetFormat(m_flServerTime), flMantissaStep).c_str(), Vars::Menu::Theme::Accent.Value);
		}
		m_flMantissaStep = flMantissaStep;

		return true;
	}

	return std::regex_match(sMsg, std::regex(R"(\d+.\d+\s\d+\s\d+)"));
}

void CNoSpreadHitscan::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (!ShouldRun(pLocal, pWeapon, true))
		return;

	m_iSeed = GetSeed(pCmd);
#ifdef SEEDPRED_DEBUG
	SDK::Output("CNoSpreadHitscan::Run", std::format("{}: {}", SDK::PlatFloatTime() + m_dTimeDelta, m_iSeed).c_str(), { 255, 0, 0 });
#endif

	if (!m_bSynced)
		return;

	// credits to cathook for average spread stuff
	float flSpread = pWeapon->GetWeaponSpread();
	int iBulletsPerShot = pWeapon->GetBulletsPerShot();
	float flFireRate = std::ceilf(pWeapon->GetFireRate() / TICK_INTERVAL) * TICK_INTERVAL;

	std::vector<Vec3> vBulletCorrections = {};
	Vec3 vAverageSpread = {};
	for (int iBullet = 0; iBullet < iBulletsPerShot; iBullet++)
	{
		SDK::RandomSeed(m_iSeed + iBullet);

		if (!iBullet) // check if we'll get a guaranteed perfect shot
		{
			// if we are doubletapping and firerate is fast enough, prioritize later bullets
			int iTicks = F::Ticks.GetTicks();
			if (!iTicks || iTicks < TIME_TO_TICKS(flFireRate) * 2)
			{
				float flTimeSinceLastShot = TICKS_TO_TIME(pLocal->m_nTickBase()) - pWeapon->m_flLastFireTime();
				if (flTimeSinceLastShot > (iBulletsPerShot > 1 ? 0.25f : 1.25f))
					return;
			}
		}

		const float x = SDK::RandomFloat(-0.5f, 0.5f) + SDK::RandomFloat(-0.5f, 0.5f);
		const float y = SDK::RandomFloat(-0.5f, 0.5f) + SDK::RandomFloat(-0.5f, 0.5f);

		Vec3 vForward, vRight, vUp; Math::AngleVectors(pCmd->viewangles, &vForward, &vRight, &vUp);
		Vec3 vFixedSpread = vForward + (vRight * x * flSpread) + (vUp * y * flSpread);
		vFixedSpread.Normalize();
		vAverageSpread += vFixedSpread;

		vBulletCorrections.push_back(vFixedSpread);
	}
	vAverageSpread /= static_cast<float>(iBulletsPerShot);

	const auto cFixedSpread = std::ranges::min_element(vBulletCorrections,
		[&](const Vec3& lhs, const Vec3& rhs)
		{
			return lhs.DistTo(vAverageSpread) < rhs.DistTo(vAverageSpread);
		});

	if (cFixedSpread == vBulletCorrections.end())
		return;

	Vec3 vFixedAngles{};
	Math::VectorAngles(*cFixedSpread, vFixedAngles);

	pCmd->viewangles += pCmd->viewangles - vFixedAngles;
	Math::ClampAngles(pCmd->viewangles);

	G::SilentAngles = true;
}

void CNoSpreadHitscan::Draw(CTFPlayer* pLocal) // i think this works?
{
	if (!(Vars::Menu::Indicators.Value & Vars::Menu::IndicatorsEnum::SeedPrediction) || !Vars::Aimbot::General::NoSpread.Value || !pLocal->IsAlive())
		return;

	auto pWeapon = H::Entities.GetWeapon();
	if (!pWeapon || !ShouldRun(pLocal, pWeapon))
		return;

	const DragBox_t dtPos = Vars::Menu::SeedPredictionDisplay.Value;
	const auto& fFont = H::Fonts.GetFont(FONT_INDICATORS);
	const int iRounding = H::Draw.Scale(3);
	const int iBottomPadding = H::Draw.Scale(4, Scale_Round);
	const int textPadding = H::Draw.Scale(4, Scale_Round);
	const int fixedWidth = H::Draw.Scale(120, Scale_Round);
	const int progressBarMargin = H::Draw.Scale(2, Scale_Round);
	const int iBarRounding = std::max(1, iRounding / 2);

	std::string mainStatusText = std::format("Uptime: {}", GetFormat(m_flServerTime));
	int textW, textH;
	I::MatSystemSurface->GetTextSize(fFont.m_dwFont, SDK::ConvertUtf8ToWide(mainStatusText.c_str()).c_str(), textW, textH);

	int w = fixedWidth;
	int h = std::max(static_cast<int>(H::Draw.Scale(24, Scale_Round)), textH + textPadding * 2) + iBottomPadding;
	int x = dtPos.x - w / 2;
	int y = dtPos.y;

	H::Draw.FillRoundRect(x, y, w, h, iRounding, Vars::Menu::Theme::Background.Value);

	int barHeight = H::Draw.Scale(2, Scale_Round);
	int barY = y + h - barHeight - iBottomPadding;
	
	float flDays = m_flServerTime / 86400.f; // bing bong math server life 
	float flRatio = std::clamp(flDays / 30.f, 0.0f, 1.0f); // 30 days baby because i dont think servers last for more than 30!!

	static float flAnimatedRatio = 0.0f;
	flAnimatedRatio = flAnimatedRatio + (flRatio - flAnimatedRatio) * std::min(I::GlobalVars->frametime * 11.3f, 1.0f);

	int totalBarWidth = w - 2 * iRounding - 2 * progressBarMargin;
	int currentBarWidth = static_cast<int>(totalBarWidth * flAnimatedRatio);

	Color_t dimmedAccent = BlendColors(Vars::Menu::Theme::Accent.Value, Vars::Menu::Theme::Background.Value, 0.5f);
	H::Draw.FillRoundRect(x + iRounding + progressBarMargin, barY, totalBarWidth, barHeight, iBarRounding, dimmedAccent);

	if (currentBarWidth > 0)
		H::Draw.FillRoundRect(x + iRounding + progressBarMargin, barY, currentBarWidth, barHeight, iBarRounding, Vars::Menu::Theme::Accent.Value);

	Color_t borderColor = BlendColors(Vars::Menu::Theme::Background.Value, Color_t(255, 255, 255, 50), 0.1f);
	H::Draw.LineRoundRect(x, y, w, h, iRounding, borderColor);

	const auto& cColor = m_bSynced ? Vars::Colors::IndicatorTextGood.Value : Vars::Colors::IndicatorTextBad.Value;

	H::Draw.StringOutlined(
		fFont,
		x + textPadding,
		y + (h - barHeight - iBottomPadding) / 2, 
		cColor,
		Vars::Menu::Theme::Background.Value.Alpha(150),
		ALIGN_LEFT,
		mainStatusText.c_str()
	);

}
