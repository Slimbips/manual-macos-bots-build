/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// playerbot.cpp: Multiplayer bot system.
//
// FIXME: Refactor code and use OOP-based state system

#include "g_local.h"
#include "actor.h"
#include "playerbot.h"
#include "consoleevent.h"
#include "debuglines.h"
#include "scriptexception.h"
#include "vehicleturret.h"
#include "weaputils.h"
#include "windows.h"
#include "g_bot.h"

// We assume that we have limited access to the server-side
// and that most logic come from the playerstate_s structure

CLASS_DECLARATION(Listener, BotController, NULL) {
    {NULL, NULL}
};

BotController::botfunc_t BotController::botfuncs[MAX_BOT_FUNCTIONS];

BotController::BotController()
{
    if (LoadingSavegame) {
        return;
    }

    m_botCmd.serverTime = 0;
    m_botCmd.msec       = 0;
    m_botCmd.buttons    = 0;
    m_botCmd.angles[0]  = ANGLE2SHORT(0);
    m_botCmd.angles[1]  = ANGLE2SHORT(0);
    m_botCmd.angles[2]  = ANGLE2SHORT(0);

    m_botCmd.forwardmove = 0;
    m_botCmd.rightmove   = 0;
    m_botCmd.upmove      = 0;

    m_botEyes.angles[0] = 0;
    m_botEyes.angles[1] = 0;
    m_botEyes.ofs[0]    = 0;
    m_botEyes.ofs[1]    = 0;
    m_botEyes.ofs[2]    = DEFAULT_VIEWHEIGHT;

    m_iCuriousTime        = 0;
    m_iAttackTime         = 0;
    m_iEnemyEyesTag       = -1;
    m_iContinuousFireTime = 0;
    m_iLastSeenTime       = 0;
    m_iLastUnseenTime     = 0;
    m_iLastBurstTime      = 0;

    m_iNextTauntTime = 0;

    // Human-like behavior initialization
    m_fHeartRate           = 50.0f;   // Normal resting heart rate
    m_iLastStressEvent     = 0;
    m_iMoraleFade          = 0;
    m_fAimInaccuracy       = 0.0f;
    m_iLastAimAdjustTime   = 0;
    m_iPanicLevel          = 0;
    m_bReloading           = false;
    m_iReloadStartTime     = 0;
    m_iHesitationTime      = 0;
    m_iLastCoverCheckTime  = 0;
    m_vLastCoverPos        = vec_zero;
    m_iEquipmentUsedTime   = 0;
    m_fFatigueLevel        = 0.0f;
    m_iLastSprintTime      = 0;
    m_bSuppressed          = false;
    m_iSuppressionEndTime  = 0;
    m_iCommunicationDelay  = 0;
    m_iLastCommunicationTime = 0;
    m_iLeanDirection       = 0;
    m_iLeanEndTime         = 0;
    m_iNextLeanDecisionTime = 0;
    m_iOrbitDirection      = (G_Random(100) < 50) ? -1 : 1;
    m_iNextOrbitSwitchTime = 0;
    m_iHoldPositionUntilTime = 0;
    m_iNextHoldCheckTime = 0;
    m_iCoverPeekState      = 0;
    m_iCoverPeekSwitchTime = 0;

    m_StateFlags = 0;
}

BotController::~BotController()
{
    if (controlledEnt) {
        controlledEnt->delegate_gotKill.Remove(delegateHandle_gotKill);
        controlledEnt->delegate_killed.Remove(delegateHandle_killed);
        controlledEnt->delegate_damage.Remove(delegateHandle_damage);
        controlledEnt->delegate_stufftext.Remove(delegateHandle_stufftext);
        controlledEnt->delegate_spawned.Remove(delegateHandle_spawned);
    }
}

BotMovement& BotController::GetMovement()
{
    return movement;
}

void BotController::Init(void)
{
    for (int i = 0; i < MAX_BOT_FUNCTIONS; i++) {
        botfuncs[i].BeginState = &BotController::State_DefaultBegin;
        botfuncs[i].EndState   = &BotController::State_DefaultEnd;
    }

    InitState_Attack(&botfuncs[0]);
    InitState_Curious(&botfuncs[1]);
    InitState_Grenade(&botfuncs[2]);
    InitState_Idle(&botfuncs[3]);
    //InitState_Weapon(&botfuncs[4]);
}

void BotController::GetUsercmd(usercmd_t *ucmd)
{
    *ucmd = m_botCmd;
}

void BotController::GetEyeInfo(usereyes_t *eyeinfo)
{
    *eyeinfo = m_botEyes;
}

void BotController::UpdateBotStates(void)
{
    m_botCmd.serverTime = level.svsTime;

    if (g_bot_manualmove->integer) {
        m_botCmd.buttons = 0;
        m_botCmd.forwardmove = m_botCmd.rightmove = m_botCmd.upmove = 0;
        return;
    }

    if (!controlledEnt->client->pers.dm_primary[0]) {
        Event *event;

        //
        // Primary weapon
        //
        event = new Event(EV_Player_PrimaryDMWeapon);
        event->AddString("auto");

        controlledEnt->ProcessEvent(event);
    }

    if (controlledEnt->GetTeam() == TEAM_NONE || controlledEnt->GetTeam() == TEAM_SPECTATOR) {
        float time;

        // Add some delay to avoid telefragging
        time = controlledEnt->entnum / 20.0;

        if (controlledEnt->EventPending(EV_Player_AutoJoinDMTeam)) {
            return;
        }

        //
        // Team
        //
        controlledEnt->PostEvent(EV_Player_AutoJoinDMTeam, time);
        return;
    }

    if (controlledEnt->IsDead() || controlledEnt->IsSpectator()) {
        // The bot should respawn
        m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
        return;
    }

    m_botCmd.buttons |= BUTTON_RUN;

    m_botEyes.ofs[0]    = 0;
    m_botEyes.ofs[1]    = 0;
    m_botEyes.ofs[2]    = controlledEnt->viewheight;
    m_botEyes.angles[0] = 0;
    m_botEyes.angles[1] = 0;

    CheckStates();

    movement.MoveThink(m_botCmd);
    rotation.TurnThink(m_botCmd, m_botEyes);
    CheckUse();

    CheckValidWeapon();
}

void BotController::CheckUse(void)
{
    Vector  dir;
    Vector  start;
    Vector  end;
    trace_t trace;

    if (controlledEnt->GetLadder()) {
        return;
    }

    controlledEnt->angles.AngleVectorsLeft(&dir);

    start = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight);
    end   = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight) + dir * 64;

    trace = G_Trace(
        start, vec_zero, vec_zero, end, controlledEnt, MASK_USABLE | MASK_LADDER, false, "BotController::CheckUse"
    );

    if (!trace.ent || trace.ent->entity == world) {
        m_botCmd.buttons &= ~BUTTON_USE;
        return;
    }

    if (trace.ent->entity->IsSubclassOfDoor()) {
        Door *door = static_cast<Door *>(trace.ent->entity);
        if (door->isOpen()) {
            // Don't use an open door
            m_botCmd.buttons &= ~BUTTON_USE;
            return;
        }
    } else if (!trace.ent->entity->isSubclassOf(FuncLadder)) {
        m_botCmd.buttons &= ~BUTTON_USE;
        return;
    }

    //
    // Toggle the use button
    //
    m_botCmd.buttons ^= BUTTON_USE;

#if 0
    Vector  forward;
    Vector  start, end;

    AngleVectors(controlledEnt->GetViewAngles(), forward, NULL, NULL);

    start = (controlledEnt->m_vViewPos - forward * 12.0f);
    end   = (controlledEnt->m_vViewPos + forward * 128.0f);

    trace = G_Trace(start, vec_zero, vec_zero, end, controlledEnt, MASK_LADDER, qfalse, "checkladder");
    if (trace.ent->entity && trace.ent->entity->isSubclassOf(FuncLadder)) {
        return;
    }

    m_botCmd.buttons ^= BUTTON_USE;
#endif
}

bool BotController::CheckWindows(void)
{
    trace_t trace;
    Vector  start, end;
    Vector  dir;

    controlledEnt->angles.AngleVectorsLeft(&dir);
    start = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight);
    end   = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight) + dir * 64;

    trace = G_Trace(start, vec_zero, vec_zero, end, controlledEnt, MASK_PLAYERSOLID, false, "BotController::CheckUse");

    if (trace.fraction != 1 && trace.ent) {
        if (trace.ent->entity->isSubclassOf(WindowObject)) {
            return true;
        }
    }

    return false;
}

void BotController::CheckValidWeapon()
{
    Weapon *weapon = controlledEnt->GetActiveWeapon(WEAPON_MAIN);
    if (!weapon) {
        // If holstered, use the best weapon available
        UseWeaponWithAmmo();
    } else if (!weapon->HasAmmo(FIRE_PRIMARY) && !controlledEnt->GetNewActiveWeapon()) {
        // In case the current weapon has no ammo, use the best available weapon
        UseWeaponWithAmmo();
    }
}

void BotController::SendCommand(const char *text)
{
    char        *buffer;
    char        *data;
    size_t       len;
    ConsoleEvent ev;

    len = strlen(text) + 1;

    buffer = (char *)gi.Malloc(len);
    data   = buffer;
    Q_strncpyz(data, text, len);

    const char *com_token = COM_Parse(&data);

    if (!com_token) {
        return;
    }

    controlledEnt->m_lastcommand = com_token;

    if (!Event::GetEvent(com_token)) {
        return;
    }

    ev = ConsoleEvent(com_token);

    if (!(ev.GetEventFlags(ev.eventnum) & EV_CONSOLE)) {
        gi.Free(buffer);
        return;
    }

    ev.SetConsoleEdict(controlledEnt->edict);

    while (1) {
        com_token = COM_Parse(&data);

        if (!com_token || !*com_token) {
            break;
        }

        ev.AddString(com_token);
    }

    gi.Free(buffer);

    try {
        controlledEnt->ProcessEvent(ev);
    } catch (ScriptException& exc) {
        gi.DPrintf("*** Bot Command Exception *** %s\n", exc.string.c_str());
    }
}

/*
====================
AimAtAimNode

Make the bot face toward the current path
====================
*/
void BotController::AimAtAimNode(void)
{
    Vector goal;

    if (!movement.IsMoving()) {
        return;
    }

    //goal = movement.GetCurrentGoal();
    //if (goal != controlledEnt->origin) {
    //    rotation.AimAt(goal);
    //}

    if (controlledEnt->GetLadder()) {
        Vector vAngles = movement.GetCurrentPathDirection().toAngles();
        vAngles.x      = Q_clamp_float(vAngles.x, -80, 80);

        rotation.SetTargetAngles(vAngles);
        return;
    } else {
        Vector targetAngles;
        targetAngles   = movement.GetCurrentPathDirection().toAngles();
        targetAngles.x = 0;
        rotation.SetTargetAngles(targetAngles);
    }
}

/*
====================
CheckReload

Make the bot reload if necessary
====================
*/
void BotController::CheckReload(void)
{
    Weapon *weap;

    if (level.inttime < m_iLastFireTime + 2000) {
        // Don't reload while attacking
        return;
    }

    weap = controlledEnt->GetActiveWeapon(WEAPON_MAIN);

    if (weap && weap->CheckReload(FIRE_PRIMARY)) {
        SendCommand("reload");
    }
}

/*
====================
NoticeEvent

Warn the bot of an event
====================
*/
void BotController::NoticeEvent(Vector vPos, int iType, Entity *pEnt, float fDistanceSquared, float fRadiusSquared)
{
    Sentient *pSentOwner;
    float     fRangeFactor;
    Vector    delta1, delta2;

    if (m_iCuriousTime) {
        delta1 = vPos - controlledEnt->origin;
        delta2 = m_vNewCuriousPos - controlledEnt->origin;
        if (delta1.lengthSquared() < delta2.lengthSquared()) {
            return;
        }
    }

    fRangeFactor = 1.0 - (fDistanceSquared / fRadiusSquared);

    if (fRangeFactor < random()) {
        return;
    }

    if (pEnt->IsSubclassOfSentient()) {
        pSentOwner = static_cast<Sentient *>(pEnt);
    } else if (pEnt->IsSubclassOfVehicleTurretGun()) {
        VehicleTurretGun *pVTG = static_cast<VehicleTurretGun *>(pEnt);
        pSentOwner             = pVTG->GetSentientOwner();
    } else if (pEnt->IsSubclassOfItem()) {
        Item *pItem = static_cast<Item *>(pEnt);
        pSentOwner  = pItem->GetOwner();
    } else if (pEnt->IsSubclassOfProjectile()) {
        Projectile *pProj = static_cast<Projectile *>(pEnt);
        pSentOwner        = pProj->GetOwner();
    } else {
        pSentOwner = NULL;
    }

    if (pSentOwner) {
        if (pSentOwner == controlledEnt) {
            // Ignore self
            return;
        }

        if ((pSentOwner->flags & FL_NOTARGET) || pSentOwner->getSolidType() == SOLID_NOT) {
            return;
        }

        // Ignore teammates
        if (pSentOwner->IsSubclassOfPlayer()) {
            Player *p = static_cast<Player *>(pSentOwner);

            if (g_gametype->integer >= GT_TEAM && p->GetTeam() == controlledEnt->GetTeam()) {
                return;
            }
        }
    }

    switch (iType) {
    case AI_EVENT_MISC:
    case AI_EVENT_MISC_LOUD:
        break;
    case AI_EVENT_WEAPON_FIRE:
    case AI_EVENT_WEAPON_IMPACT:
    case AI_EVENT_EXPLOSION:
    case AI_EVENT_AMERICAN_VOICE:
    case AI_EVENT_GERMAN_VOICE:
    case AI_EVENT_AMERICAN_URGENT:
    case AI_EVENT_GERMAN_URGENT:
    case AI_EVENT_FOOTSTEP:
    case AI_EVENT_GRENADE:
    default:
        m_iCuriousTime   = level.inttime + 20000;
        m_vNewCuriousPos = vPos;
        break;
    }
}

/*
====================
ClearEnemy

Clear the bot's enemy
====================
*/
void BotController::ClearEnemy(void)
{
    m_iAttackTime   = 0;
    m_pEnemy        = NULL;
    m_iEnemyEyesTag = -1;
    m_vOldEnemyPos  = vec_zero;
    m_vLastEnemyPos = vec_zero;
}

/*
====================
Bot states
--------------------
____________________
--------------------
____________________
--------------------
____________________
--------------------
____________________
====================
*/

void BotController::CheckStates(void)
{
    m_StateCount = 0;

    for (int i = 0; i < MAX_BOT_FUNCTIONS; i++) {
        botfunc_t *func = &botfuncs[i];

        if (func->CheckCondition) {
            if ((this->*func->CheckCondition)()) {
                if (!(m_StateFlags & (1 << i))) {
                    m_StateFlags |= 1 << i;

                    if (func->BeginState) {
                        (this->*func->BeginState)();
                    }
                }

                if (func->ThinkState) {
                    m_StateCount++;
                    (this->*func->ThinkState)();
                }
            } else {
                if ((m_StateFlags & (1 << i))) {
                    m_StateFlags &= ~(1 << i);

                    if (func->EndState) {
                        (this->*func->EndState)();
                    }
                }
            }
        } else {
            if (func->ThinkState) {
                m_StateCount++;
                (this->*func->ThinkState)();
            }
        }
    }

    assert(m_StateCount);
    if (!m_StateCount) {
        gi.DPrintf("*** WARNING *** %s was stuck with no states !!!", controlledEnt->client->pers.netname);
        State_Reset();
    }
}

/*
====================
Default state


====================
*/
void BotController::State_DefaultBegin(void)
{
    movement.ClearMove();
}

void BotController::State_DefaultEnd(void) {}

void BotController::State_Reset(void)
{
    m_iCuriousTime    = 0;
    m_iAttackTime     = 0;
    m_vLastCuriousPos = vec_zero;
    m_vOldEnemyPos    = vec_zero;
    m_vLastEnemyPos   = vec_zero;
    m_vLastDeathPos   = vec_zero;
    m_pEnemy          = NULL;
    m_iEnemyEyesTag   = -1;
}

/*
====================
Idle state

Make the bot move to random directions
====================
*/
void BotController::InitState_Idle(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Idle;
    func->ThinkState     = &BotController::State_Idle;
}

bool BotController::CheckCondition_Idle(void)
{
    if (m_iCuriousTime) {
        return false;
    }

    if (m_iAttackTime) {
        return false;
    }

    return true;
}

void BotController::State_Idle(void)
{
    // Gradually recover from stress when idle
    m_fHeartRate = Q_max(50.0f, m_fHeartRate - 10.0f * level.frametime);
    m_iPanicLevel = Q_max(0, m_iPanicLevel - 8);
    m_fFatigueLevel = Q_max(0.0f, m_fFatigueLevel - 0.2f * level.frametime);

    if (CheckWindows()) {
        m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
        m_iLastFireTime = level.inttime;
    } else {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        CheckReload();
    }

    AimAtAimNode();

    if (!movement.MoveToBestAttractivePoint() && !movement.IsMoving()) {
        if (m_vLastDeathPos != vec_zero) {
            movement.MoveTo(m_vLastDeathPos);

            if (movement.MoveDone()) {
                m_vLastDeathPos = vec_zero;
            }
        } else {
            Vector randomDir(G_CRandom(16), G_CRandom(16), G_CRandom(16));
            Vector preferredDir;
            float  radius = 512 + G_Random(2048);

            preferredDir += Vector(controlledEnt->orientation[0]) * (rand() % 5 ? 1024 : -1024);
            preferredDir += Vector(controlledEnt->orientation[2]) * (rand() % 5 ? 1024 : -1024);
            movement.AvoidPath(controlledEnt->origin + randomDir, radius, preferredDir);
        }
    }
}

/*
====================
Curious state

Forward to the last event position
====================
*/
void BotController::InitState_Curious(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Curious;
    func->ThinkState     = &BotController::State_Curious;
}

bool BotController::CheckCondition_Curious(void)
{
    if (m_iAttackTime) {
        m_iCuriousTime = 0;
        return false;
    }

    if (level.inttime > m_iCuriousTime) {
        if (m_iCuriousTime) {
            movement.ClearMove();
            m_iCuriousTime = 0;
        }

        return false;
    }

    return true;
}

void BotController::State_Curious(void)
{
    // Maintain awareness when being curious
    UpdateStressLevel();

    if (CheckWindows()) {
        m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
        m_iLastFireTime = level.inttime;
    } else {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    }

    AimAtAimNode();

    if (!movement.MoveToBestAttractivePoint(3) && (!movement.IsMoving() || m_vLastCuriousPos != m_vNewCuriousPos)) {
        movement.MoveTo(m_vNewCuriousPos);
        m_vLastCuriousPos = m_vNewCuriousPos;
    }

    if (movement.MoveDone()) {
        m_iCuriousTime = 0;
    }
}

/*
====================
Attack state

Attack the enemy
====================
*/
void BotController::InitState_Attack(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Attack;
    func->EndState       = &BotController::State_EndAttack;
    func->ThinkState     = &BotController::State_Attack;
}

static Vector bot_origin;

static int sentients_compare(const void *elem1, const void *elem2)
{
    Entity *e1, *e2;
    float   delta[3];
    float   d1, d2;

    e1 = *(Entity **)elem1;
    e2 = *(Entity **)elem2;

    VectorSubtract(bot_origin, e1->origin, delta);
    d1 = VectorLengthSquared(delta);

    VectorSubtract(bot_origin, e2->origin, delta);
    d2 = VectorLengthSquared(delta);

    if (d2 <= d1) {
        return d1 > d2;
    } else {
        return -1;
    }
}

static int ComputeCornerAwareLeanDirection(Player *botPlayer, const Vector& enemyPos, int fallbackDirection)
{
    Vector vRight = Vector(botPlayer->orientation[1]);
    vRight.z      = 0;

    if (vRight.lengthSquared() <= Square(0.01f)) {
        return fallbackDirection;
    }

    VectorNormalize2D(vRight);

    const Vector eyePos = botPlayer->EyePosition();
    const float  sideProbeDist = 42.0f;

    trace_t leftTrace = G_Trace(
        eyePos,
        vec_zero,
        vec_zero,
        eyePos - vRight * sideProbeDist,
        botPlayer,
        MASK_PLAYERSOLID,
        qtrue,
        "BotLeanLeftProbe"
    );

    trace_t rightTrace = G_Trace(
        eyePos,
        vec_zero,
        vec_zero,
        eyePos + vRight * sideProbeDist,
        botPlayer,
        MASK_PLAYERSOLID,
        qtrue,
        "BotLeanRightProbe"
    );

    const bool leftBlocked  = leftTrace.fraction < 0.72f;
    const bool rightBlocked = rightTrace.fraction < 0.72f;

    if (rightBlocked && !leftBlocked) {
        // Right side close to frame/wall -> lean left to peek around corner
        return -1;
    }

    if (leftBlocked && !rightBlocked) {
        return 1;
    }

    return fallbackDirection;
}

bool BotController::IsValidEnemy(Sentient *sent) const
{
    if (sent == controlledEnt) {
        return false;
    }

    if (sent->hidden() || (sent->flags & FL_NOTARGET)) {
        // Ignore hidden / non-target enemies
        return false;
    }

    if (sent->IsDead()) {
        // Ignore dead enemies
        return false;
    }

    if (sent->getSolidType() == SOLID_NOT) {
        // Ignore non-solid, like spectators
        return false;
    }

    if (sent->IsSubclassOfPlayer()) {
        Player *player = static_cast<Player *>(sent);

        if (g_gametype->integer >= GT_TEAM && player->GetTeam() == controlledEnt->GetTeam()) {
            return false;
        }
    } else {
        if (sent->m_Team == controlledEnt->m_Team) {
            return false;
        }
    }

    return true;
}

bool BotController::CheckCondition_Attack(void)
{
    Container<Sentient *> sents       = SentientList;
    float                 maxDistance = 0;

    bot_origin = controlledEnt->origin;
    sents.Sort(sentients_compare);

    for (int i = 1; i <= sents.NumObjects(); i++) {
        Sentient *sent = sents.ObjectAt(i);

        if (!IsValidEnemy(sent)) {
            continue;
        }

        maxDistance = Q_min(world->m_fAIVisionDistance * 1.35f, world->farplane_distance);

        if (controlledEnt->CanSee(sent, 180, maxDistance, false)) {
            if (m_pEnemy != sent) {
                m_iEnemyEyesTag = -1;
            }

            if (!m_pEnemy) {
                m_iLastUnseenTime = level.inttime;
            }

            m_pEnemy        = sent;
            m_vLastEnemyPos = m_pEnemy->origin;
        }

        if (m_pEnemy) {
            m_iAttackTime = level.inttime + 1000;
            return true;
        }
    }

    if (level.inttime > m_iAttackTime) {
        if (m_iAttackTime) {
            movement.ClearMove();
            m_iAttackTime = 0;
        }

        return false;
    }

    return true;
}

void BotController::State_EndAttack(void)
{
    m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    m_botCmd.buttons &= ~(BUTTON_LEAN_LEFT | BUTTON_LEAN_RIGHT);
    m_iLeanDirection = 0;
    m_iLeanEndTime = 0;
    m_iNextOrbitSwitchTime = 0;
    m_iHoldPositionUntilTime = 0;
    m_iCoverPeekState = 0;
    m_iCoverPeekSwitchTime = 0;
    controlledEnt->ZoomOff();
}

void BotController::State_Attack(void)
{
    bool    bMelee              = false;
    bool    bCanSee             = false;
    bool    bCanAttack          = false;
    float   fMinDistance        = 128;
    float   fMinDistanceSquared = fMinDistance * fMinDistance;
    float   fEnemyDistanceSquared;
    bool    bCloseRange = false;
    bool    bPointBlank = false;
    bool    bRecentlyHit = false;
    Weapon *pWeap   = controlledEnt->GetActiveWeapon(WEAPON_MAIN);
    bool    bNoMove = false;
    bool    bFiring = false;

    // Lean is controlled by this state only
    m_botCmd.buttons &= ~(BUTTON_LEAN_LEFT | BUTTON_LEAN_RIGHT);

    // Update human-like behavior systems
    UpdateStressLevel();
    UpdatePanicLevel();
    UpdateFatigue();
    UpdateSuppression();
    UpdateReloadBehavior();
    CheckForCover();

    // Debug: log current important state for firing decisions
    const char *enemyName = "none";
    if (m_pEnemy) {
        if (m_pEnemy->IsSubclassOfPlayer()) {
            enemyName = "player";
        } else {
            enemyName = "entity";
        }
    }
    gi.DPrintf("[BOTDBG] %s AttackState: enemy=%s canSee=%d reloading=%d panic=%d hesitation=%d aimInacc=%.2f\n",
               controlledEnt->client->pers.netname, enemyName,
               (int)bCanSee, (int)m_bReloading, m_iPanicLevel, (int)Q_max(0, m_iHesitationTime - level.inttime), m_fAimInaccuracy);

    if (!m_pEnemy || !IsValidEnemy(m_pEnemy)) {
        // Ignore dead enemies
        m_iAttackTime = 0;
        return;
    }
    float fDistanceSquared = (m_pEnemy->origin - controlledEnt->origin).lengthSquared();
    bCloseRange = fDistanceSquared < Square(320.0f);
    bPointBlank = fDistanceSquared < Square(160.0f);
    bRecentlyHit = level.inttime <= controlledEnt->m_iLastHitTime + 900;

    m_vOldEnemyPos = m_vLastEnemyPos;

    const float attackFov = bPointBlank ? 180.0f : (bCloseRange ? 140.0f : 160.0f);
    const float attackVisionDistance = Q_min(world->m_fAIVisionDistance * 1.25f, world->farplane_distance);
    bCanSee = controlledEnt->CanSee(
        m_pEnemy, attackFov, attackVisionDistance, false
    );

    if (bCanSee) {
        if (!pWeap) {
            return;
        }

        bCanAttack = true;
        if (m_iLastUnseenTime) {
            const unsigned int minDelay = g_bot_attack_react_min_delay->value * 1000;
            const unsigned int randomDelay = g_bot_attack_react_random_delay->value * 1000;
            
            // Add nervous hesitation based on panic level
            unsigned int hesitationDelay = minDelay + G_Random(randomDelay);
            hesitationDelay += (m_iPanicLevel / 100.0f) * 200;  // Smaller extra delay when panicked
            if (bCloseRange) {
                hesitationDelay = hesitationDelay / 3;
            }
            if (bRecentlyHit) {
                hesitationDelay = Q_max(40u, hesitationDelay / 4);
            }
            
            if (level.inttime <= m_iLastUnseenTime + hesitationDelay) {
                bCanAttack = false;
            } else {
                m_iLastUnseenTime = 0;
            }
        }

        if (bCanAttack) {
            const int fireDelay                    = pWeap->FireDelay(FIRE_PRIMARY) * 1000;
            float     fPrimaryBulletRange          = pWeap->GetBulletRange(FIRE_PRIMARY) / 1.25f;
            float     fPrimaryBulletRangeSquared   = fPrimaryBulletRange * fPrimaryBulletRange;
            float     fSecondaryBulletRange        = pWeap->GetBulletRange(FIRE_SECONDARY);
            float     fSecondaryBulletRangeSquared = fSecondaryBulletRange * fSecondaryBulletRange;

            const int maxcontinuousFireTime = fireDelay + g_bot_attack_continuousfire_min_firetime->value * 1000
                                           + G_Random(g_bot_attack_continuousfire_random_firetime->value * 1000);
            const int maxBurstTime = fireDelay + g_bot_attack_burst_min_time->value * 1000
                                   + G_Random(g_bot_attack_burst_random_delay->value * 1000);

            //
            // check the fire movement speed if the weapon has a max fire movement
            //
            if (pWeap->GetMaxFireMovement() < 1 && pWeap->HasAmmoInClip(FIRE_PRIMARY)) {
                float length;

                length = controlledEnt->velocity.length();
                if ((length / sv_runspeed->value) > (pWeap->GetMaxFireMovementMult())) {
                    bNoMove = true;
                    movement.ClearMove();
                }
            }

            fMinDistance = fPrimaryBulletRange;

            if (fMinDistance > 128) {
                fMinDistance = 128;
            }

            if (bCloseRange) {
                fMinDistance = 48;
            }

            fMinDistanceSquared = fMinDistance * fMinDistance;

            if (controlledEnt->client->ps.stats[STAT_AMMO] <= 0
                && controlledEnt->client->ps.stats[STAT_CLIPAMMO] <= 0) {
                m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
                controlledEnt->ZoomOff();
            } else if (fDistanceSquared > fPrimaryBulletRangeSquared) {
                m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
                controlledEnt->ZoomOff();
            } else {
                //
                // Attacking
                //

                if (pWeap->IsSemiAuto()) {
                    if (controlledEnt->client->ps.iViewModelAnim != VM_ANIM_IDLE
                        && (controlledEnt->client->ps.iViewModelAnim < VM_ANIM_IDLE_0
                            || controlledEnt->client->ps.iViewModelAnim > VM_ANIM_IDLE_2)) {
                        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
                        controlledEnt->ZoomOff();
                    } else if (bCloseRange || bRecentlyHit || level.inttime > m_iHesitationTime) {
                        // Check hesitation - bots sometimes hesitate under stress
                        if (bCloseRange || bRecentlyHit || m_iPanicLevel < G_Random(120)) {
                            bFiring = true;
                            m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
                            if (pWeap->GetZoom()) {
                                if (!controlledEnt->IsZoomed()) {
                                    m_botCmd.buttons |= BUTTON_ATTACKRIGHT;
                                } else {
                                    m_botCmd.buttons &= ~BUTTON_ATTACKRIGHT;
                                }
                            }
                            gi.DPrintf("[BOTDBG] %s decides to fire (semi-auto).\n", controlledEnt->client->pers.netname);
                        } else {
                            // Hesitation - don't fire this frame
                            m_iHesitationTime = level.inttime + G_Random(120) + 60;
                            gi.DPrintf("[BOTDBG] %s hesitates until %d (panic=%d).\n", controlledEnt->client->pers.netname, m_iHesitationTime, m_iPanicLevel);
                        }
                    } else {
                        bNoMove = true;
                        movement.ClearMove();
                    }
                } else {
                    // For automatic weapons, simulate realistic bursts
                    if ((bCloseRange || bRecentlyHit || level.inttime > m_iHesitationTime) && !m_bReloading && (!m_bSuppressed || bCloseRange || bRecentlyHit)) {
                        bFiring = true;
                        m_botCmd.buttons |= BUTTON_ATTACKLEFT;
                        gi.DPrintf("[BOTDBG] %s fires (auto).\n", controlledEnt->client->pers.netname);
                    }
                }
            }

            // Contextual leaning during firefights (also possible while running)
            if (bCanSee && !bPointBlank && fDistanceSquared < Square(1800.0f) && !m_bReloading) {
                const bool bIsMovingFast = controlledEnt->velocity.length() > sv_runspeed->value * 0.4f;
                if (level.inttime >= m_iNextLeanDecisionTime) {
                    const bool bWantLean = m_bSuppressed || bRecentlyHit || (G_Random(100) < (bIsMovingFast ? 25 : 45));
                    if (bWantLean) {
                        Vector vToEnemy = m_pEnemy->origin - controlledEnt->origin;
                        const float side = DotProduct(vToEnemy, Vector(controlledEnt->orientation[1]));
                        int leanDir = 0;

                        if (fabsf(side) > 4.0f) {
                            leanDir = side > 0.0f ? 1 : -1;
                        } else {
                            leanDir = (G_Random(100) < 50) ? -1 : 1;
                        }

                        m_iLeanDirection = ComputeCornerAwareLeanDirection(controlledEnt, m_pEnemy->origin, leanDir);

                        m_iLeanEndTime = level.inttime + (bIsMovingFast ? 260 : 420) + G_Random(bIsMovingFast ? 260 : 420);
                    } else {
                        m_iLeanDirection = 0;
                        m_iLeanEndTime = 0;
                    }

                    m_iNextLeanDecisionTime = level.inttime + (bIsMovingFast ? 320 : 520) + G_Random(bIsMovingFast ? 320 : 520);
                }

                if (m_iLeanDirection && level.inttime < m_iLeanEndTime) {
                    if (m_iLeanDirection < 0) {
                        m_botCmd.buttons |= BUTTON_LEAN_LEFT;
                    } else {
                        m_botCmd.buttons |= BUTTON_LEAN_RIGHT;
                    }
                }
            } else {
                m_iLeanDirection = 0;
                m_iLeanEndTime = 0;
            }

            //
            // Burst
            //

            if (!pWeap->IsSemiAuto()) {
                if (m_iLastBurstTime) {
                    if (level.inttime > m_iLastBurstTime + maxBurstTime) {
                        m_iLastBurstTime      = 0;
                        m_iContinuousFireTime = 0;
                    } else {
                        m_botCmd.buttons &= ~BUTTON_ATTACKLEFT;
                    }
                } else {
                    if (bFiring) {
                        m_iContinuousFireTime += level.intframetime;
                    } else {
                        m_iContinuousFireTime = 0;
                    }

                    if (!m_iLastBurstTime && m_iContinuousFireTime > maxcontinuousFireTime) {
                        m_iLastBurstTime      = level.inttime;
                        m_iContinuousFireTime = 0;
                    }
                }
            } else {
                // Semi-auto should not be paused by automatic burst logic
                m_iLastBurstTime      = 0;
                m_iContinuousFireTime = 0;
            }

            m_iLastFireTime = level.inttime;

            if (pWeap->GetFireType(FIRE_SECONDARY) == FT_MELEE) {
                if (controlledEnt->client->ps.stats[STAT_AMMO] <= 0
                    && controlledEnt->client->ps.stats[STAT_CLIPAMMO] <= 0) {
                    bMelee = true;
                } else if (fDistanceSquared <= fSecondaryBulletRangeSquared) {
                    bMelee = true;
                }
            }

            if (bMelee) {
                m_botCmd.buttons &= ~BUTTON_ATTACKLEFT;

                if (fDistanceSquared <= fSecondaryBulletRangeSquared) {
                    m_botCmd.buttons ^= BUTTON_ATTACKRIGHT;
                } else {
                    m_botCmd.buttons &= ~BUTTON_ATTACKRIGHT;
                }
            }

            m_iAttackTime        = level.inttime + 1000;
            m_iAttackStopAimTime = level.inttime + 3000;
            m_iLastSeenTime      = level.inttime;
            m_vLastEnemyPos      = m_pEnemy->origin;
        }
    } else {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        fMinDistanceSquared = 0;

        // Peek corners when enemy was seen recently but is now just out of sight
        if (!bPointBlank && m_vLastEnemyPos != vec_zero && level.inttime < m_iLastSeenTime + 1800) {
            if (level.inttime >= m_iNextLeanDecisionTime) {
                Vector vToLastEnemy = m_vLastEnemyPos - controlledEnt->origin;
                const float side = DotProduct(vToLastEnemy, Vector(controlledEnt->orientation[1]));
                int leanDir = 0;

                if (fabsf(side) > 4.0f) {
                    leanDir = side > 0.0f ? 1 : -1;
                } else {
                    leanDir = (G_Random(100) < 50) ? -1 : 1;
                }

                m_iLeanDirection = ComputeCornerAwareLeanDirection(controlledEnt, m_vLastEnemyPos, leanDir);

                m_iLeanEndTime = level.inttime + 360 + G_Random(420);
                m_iNextLeanDecisionTime = level.inttime + 460 + G_Random(420);
            }

            if (m_iLeanDirection && level.inttime < m_iLeanEndTime) {
                if (m_iLeanDirection < 0) {
                    m_botCmd.buttons |= BUTTON_LEAN_LEFT;
                } else {
                    m_botCmd.buttons |= BUTTON_LEAN_RIGHT;
                }
            }
        } else {
            m_iLeanDirection = 0;
            m_iLeanEndTime = 0;
        }

        if (level.inttime > m_iLastSeenTime + 2000) {
            m_iLastUnseenTime = level.inttime;
        }
    }

    if (bCanSee || level.inttime < m_iAttackStopAimTime) {
        Vector        vRandomOffset;
        Vector        vTarget;
        orientation_t eyes_or;

        if (m_iEnemyEyesTag == -1) {
            // Cache the tag
            m_iEnemyEyesTag = gi.Tag_NumForName(m_pEnemy->edict->tiki, "eyes bone");
        }

        if (m_iEnemyEyesTag != -1) {
            // Use the enemy's eyes bone
            m_pEnemy->GetTag(m_iEnemyEyesTag, &eyes_or);
            vTarget = eyes_or.origin;
        } else {
            vTarget = m_pEnemy->origin;
        }

        // Apply aim inaccuracy based on stress, fatigue, and panic
        ApplyAimInaccuracy(vTarget);

        if (level.inttime >= m_iLastAimTime + 100) {
            if (m_iEnemyEyesTag != -1) {
                m_vAimOffset[0] = G_CRandom((m_pEnemy->maxs.x - m_pEnemy->mins.x) * 0.5);
                m_vAimOffset[1] = G_CRandom((m_pEnemy->maxs.y - m_pEnemy->mins.y) * 0.5);
                m_vAimOffset[2] = -G_Random(m_pEnemy->maxs.z * 0.5);
            } else {
                m_vAimOffset[0] = G_CRandom((m_pEnemy->maxs.x - m_pEnemy->mins.x) * 0.5);
                m_vAimOffset[1] = G_CRandom((m_pEnemy->maxs.y - m_pEnemy->mins.y) * 0.5);
                m_vAimOffset[2] = 16 + G_Random(m_pEnemy->viewheight - 16);
            }
            m_iLastAimTime = level.inttime;
        }

        rotation.AimAt(vTarget + m_vAimOffset * g_bot_attack_spreadmult->value);
    } else {
        AimAtAimNode();
    }

    if (bNoMove) {
        return;
    }

    if (m_bReloading && m_pEnemy) {
        if (controlledEnt->GetMoveResult() >= MOVERESULT_BLOCKED) {
            m_iOrbitDirection = -m_iOrbitDirection;
        }

        const float retreatRadius = bCloseRange ? 240.0f : 180.0f;
        Vector retreatSide = Vector(controlledEnt->orientation[1]) * (m_iOrbitDirection > 0 ? 1.0f : -1.0f);

        movement.AvoidPath(m_pEnemy->origin, retreatRadius, retreatSide * 700.0f);
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    if (level.inttime < m_iHoldPositionUntilTime) {
        if (!bPointBlank && !bRecentlyHit) {
            // While holding, often do a timed 1.5s left/right weave to avoid being static.
            if (level.inttime >= m_iNextOrbitSwitchTime) {
                m_iOrbitDirection      = -m_iOrbitDirection;
                m_iNextOrbitSwitchTime = level.inttime + 1500;
            }

            if (G_Random(100) < 80 && m_pEnemy) {
                Vector vSide = Vector(controlledEnt->orientation[1]) * (m_iOrbitDirection > 0 ? 1.0f : -1.0f);
                Vector vToEnemy = m_pEnemy->origin - controlledEnt->origin;
                vToEnemy.z = 0;

                if (vToEnemy.lengthSquared() > 0.001f) {
                    VectorNormalize2D(vToEnemy);
                } else {
                    vToEnemy = vec_zero;
                }

                Vector vHoldWeaveTarget = controlledEnt->origin + vSide * 185.0f - vToEnemy * 22.0f;
                movement.MoveNear(vHoldWeaveTarget, 130.0f);

                m_iLeanDirection = (m_iOrbitDirection < 0) ? -1 : 1;
                m_iLeanEndTime   = m_iNextOrbitSwitchTime;

                if (m_iLeanDirection < 0) {
                    m_botCmd.buttons |= BUTTON_LEAN_LEFT;
                } else {
                    m_botCmd.buttons |= BUTTON_LEAN_RIGHT;
                }
            } else {
                movement.ClearMove();
                m_botCmd.forwardmove = 0;
                m_botCmd.rightmove   = 0;
            }

            m_iAttackTime        = level.inttime + 1000;
            return;
        }

        m_iHoldPositionUntilTime = 0;
    }

    if (level.inttime >= m_iNextHoldCheckTime && bCanSee && !bPointBlank) {
        Vector vToEnemy = m_pEnemy->origin - controlledEnt->origin;
        vToEnemy.z      = 0;

        if (vToEnemy.lengthSquared() > Square(64.0f)) {
            Vector vForward = Vector(controlledEnt->orientation[0]);
            Vector vRight   = Vector(controlledEnt->orientation[1]);
            vForward.z = 0;
            vRight.z   = 0;

            if (vForward.lengthSquared() > 0.01f && vRight.lengthSquared() > 0.01f) {
                VectorNormalize2D(vToEnemy);
                VectorNormalize2D(vForward);
                VectorNormalize2D(vRight);

                const float enemyFrontDot = DotProduct(vForward, vToEnemy);
                const float enemyDistSq = (m_pEnemy->origin - controlledEnt->origin).lengthSquared();

                const Vector eyePos = controlledEnt->EyePosition();
                trace_t leftTrace = G_Trace(
                    eyePos,
                    vec_zero,
                    vec_zero,
                    eyePos - vRight * 52.0f,
                    controlledEnt,
                    MASK_PLAYERSOLID,
                    qtrue,
                    "BotHoldLeft"
                );
                trace_t rightTrace = G_Trace(
                    eyePos,
                    vec_zero,
                    vec_zero,
                    eyePos + vRight * 52.0f,
                    controlledEnt,
                    MASK_PLAYERSOLID,
                    qtrue,
                    "BotHoldRight"
                );

                trace_t forwardTrace = G_Trace(
                    eyePos,
                    vec_zero,
                    vec_zero,
                    eyePos + vForward * 96.0f,
                    controlledEnt,
                    MASK_PLAYERSOLID,
                    qtrue,
                    "BotHoldForward"
                );

                const Vector backStart = controlledEnt->origin + Vector(0, 0, 24);
                trace_t backTrace = G_Trace(
                    backStart,
                    controlledEnt->mins,
                    controlledEnt->maxs,
                    backStart - vForward * 180.0f,
                    controlledEnt,
                    MASK_PLAYERSOLID,
                    qtrue,
                    "BotHoldBack"
                );

                const bool bNarrowPass = leftTrace.fraction < 0.86f && rightTrace.fraction < 0.86f;
                const bool bHasSideCover = leftTrace.fraction < 0.90f || rightTrace.fraction < 0.90f;
                const bool bRearSafeEnough = backTrace.fraction < 0.90f;
                const bool bRangeOkay = enemyDistSq > Square(220.0f) && enemyDistSq < Square(4200.0f);
                const bool bDoorwaySlot = leftTrace.fraction < 0.80f && rightTrace.fraction < 0.80f
                                        && forwardTrace.fraction > 0.78f;

                if (bDoorwaySlot && enemyFrontDot > 0.05f && bRearSafeEnough && bRangeOkay && G_Random(100) < 93) {
                    m_iHoldPositionUntilTime = level.inttime + 1700 + G_Random(1200);
                    movement.ClearMove();
                    m_botCmd.forwardmove = 0;
                    m_botCmd.rightmove   = 0;
                    m_iAttackTime        = level.inttime + 1000;
                    m_iNextHoldCheckTime = level.inttime + 1400 + G_Random(1000);
                    return;
                }

                int holdChance = 20;
                if (bHasSideCover) {
                    holdChance += 32;
                }
                if (bNarrowPass) {
                    holdChance += 26;
                }
                if (bFiring) {
                    holdChance += 8;
                }
                if (!bCloseRange) {
                    holdChance += 6;
                }
                if (m_bSuppressed) {
                    holdChance -= 20;
                }

                holdChance = Q_clamp(holdChance, 20, 96);

                if (enemyFrontDot > -0.05f && bRearSafeEnough && bRangeOkay
                    && (bHasSideCover || bNarrowPass) && G_Random(100) < holdChance) {
                    m_iHoldPositionUntilTime = level.inttime + 1400 + G_Random(1100);
                    movement.ClearMove();
                    m_botCmd.forwardmove = 0;
                    m_botCmd.rightmove   = 0;
                    m_iAttackTime        = level.inttime + 1000;
                    m_iNextHoldCheckTime = level.inttime + 950 + G_Random(900);
                    return;
                }
            }
        }

        m_iNextHoldCheckTime = level.inttime + 220;
    }

    if (bCanSee && !bCloseRange) {
        if (level.inttime >= m_iNextOrbitSwitchTime) {
            if (G_Random(100) < 30 || controlledEnt->GetMoveResult() >= MOVERESULT_BLOCKED) {
                m_iOrbitDirection = -m_iOrbitDirection;
            }

            m_iNextOrbitSwitchTime = level.inttime + 700 + G_Random(900);
        }

        const float fStandoffDistance = 440.0f;
        const float fStandoffDistanceSq = Square(fStandoffDistance);
        const float fTooCloseSq = Square(300.0f);
        const float fTooFarSq = Square(1400.0f);

        Vector vToEnemy = m_pEnemy->origin - controlledEnt->origin;
        if (vToEnemy.lengthSquared() > 0.001f) {
            VectorNormalizeFast(vToEnemy);
        }

        Vector vSide = Vector(controlledEnt->orientation[1]) * (m_iOrbitDirection > 0 ? 1.0f : -1.0f);

        Vector vForward = Vector(controlledEnt->orientation[0]);
        Vector vRight   = Vector(controlledEnt->orientation[1]);
        vForward.z      = 0;
        vRight.z        = 0;
        if (vForward.lengthSquared() > 0.01f) {
            VectorNormalize2D(vForward);
        }
        if (vRight.lengthSquared() > 0.01f) {
            VectorNormalize2D(vRight);
        }

        const Vector eyePos = controlledEnt->EyePosition();
        trace_t leftTrace = G_Trace(
            eyePos,
            vec_zero,
            vec_zero,
            eyePos - vRight * 58.0f,
            controlledEnt,
            MASK_PLAYERSOLID,
            qtrue,
            "BotRangedLeftCover"
        );
        trace_t rightTrace = G_Trace(
            eyePos,
            vec_zero,
            vec_zero,
            eyePos + vRight * 58.0f,
            controlledEnt,
            MASK_PLAYERSOLID,
            qtrue,
            "BotRangedRightCover"
        );
        trace_t forwardTrace = G_Trace(
            eyePos,
            vec_zero,
            vec_zero,
            eyePos + vForward * 110.0f,
            controlledEnt,
            MASK_PLAYERSOLID,
            qtrue,
            "BotRangedForward"
        );

        const bool bLeftCover  = leftTrace.fraction < 0.90f;
        const bool bRightCover = rightTrace.fraction < 0.90f;
        const bool bDoorCenter = bLeftCover && bRightCover && forwardTrace.fraction > 0.82f;

        if (bDoorCenter) {
            // Avoid standing in center lane of a doorway/opening.
            Vector vJambTarget = controlledEnt->origin + vSide * 170.0f - vToEnemy * 40.0f;
            movement.MoveNear(vJambTarget, 95.0f);
            return;
        }

        if ((bLeftCover || bRightCover) && !m_bReloading) {
            int leanDir = 0;
            if (bRightCover && !bLeftCover) {
                leanDir = -1;
            } else if (bLeftCover && !bRightCover) {
                leanDir = 1;
            } else {
                leanDir = ComputeCornerAwareLeanDirection(controlledEnt, m_pEnemy->origin, m_iOrbitDirection > 0 ? 1 : -1);
            }

            if (!m_iCoverPeekSwitchTime || level.inttime >= m_iCoverPeekSwitchTime) {
                m_iCoverPeekState = 1 - m_iCoverPeekState;
                if (m_iCoverPeekState) {
                    m_iCoverPeekSwitchTime = level.inttime + 500 + G_Random(420);
                } else {
                    m_iCoverPeekSwitchTime = level.inttime + 520 + G_Random(520);
                }
            }

            const float coverSideSign = (leanDir < 0) ? 1.0f : -1.0f;

            if (m_iCoverPeekState) {
                m_iLeanDirection         = leanDir;
                m_iLeanEndTime           = level.inttime + 460 + G_Random(360);
                m_iNextLeanDecisionTime  = level.inttime + 500 + G_Random(360);

                if (m_iLeanDirection < 0) {
                    m_botCmd.buttons |= BUTTON_LEAN_LEFT;
                } else if (m_iLeanDirection > 0) {
                    m_botCmd.buttons |= BUTTON_LEAN_RIGHT;
                }

                Vector vPeekAnchor = controlledEnt->origin + vRight * (coverSideSign * 85.0f) - vToEnemy * 28.0f;
                movement.MoveNear(vPeekAnchor, 82.0f);
            } else {
                m_iLeanDirection = 0;
                m_iLeanEndTime   = 0;

                Vector vHideAnchor = controlledEnt->origin + vRight * (coverSideSign * 128.0f) - vToEnemy * 82.0f;
                movement.MoveNear(vHideAnchor, 86.0f);
            }

            m_iAttackTime = level.inttime + 1000;
            return;
        }

        m_iCoverPeekState = 0;
        m_iCoverPeekSwitchTime = 0;

        if (fDistanceSquared < fTooCloseSq) {
            // Back up if enemy got too close for ranged posture
            movement.AvoidPath(m_pEnemy->origin, fStandoffDistance, vSide * 512.0f);
            return;
        }

        if (fDistanceSquared > fTooFarSq) {
            // Close distance a bit, but don't hard-rush straight in
            Vector vApproachTarget = m_pEnemy->origin - vToEnemy * fStandoffDistance + vSide * 160.0f;
            movement.MoveNear(vApproachTarget, 140.0f);
            return;
        }

        const bool bMediumRange = fDistanceSquared >= Square(300.0f) && fDistanceSquared <= Square(1500.0f);
        if (bMediumRange) {
            // Alternate strafe side every 1.0 - 1.5 sec for readable medium-range movement.
            if (level.inttime >= m_iNextOrbitSwitchTime) {
                m_iOrbitDirection = -m_iOrbitDirection;
                const int strafeDuration = 1000 + G_Random(500);
                m_iNextOrbitSwitchTime = level.inttime + strafeDuration;

                // Match lean direction and duration with current strafe side.
                m_iLeanDirection = (m_iOrbitDirection < 0) ? -1 : 1;
                m_iLeanEndTime = m_iNextOrbitSwitchTime;
            }

            Vector vTimedSide = Vector(controlledEnt->orientation[1]) * (m_iOrbitDirection > 0 ? 1.0f : -1.0f);
            Vector vTimedStrafeTarget = controlledEnt->origin + vTimedSide * 210.0f;
            movement.MoveNear(vTimedStrafeTarget, 130.0f);

            if (m_iLeanDirection && level.inttime < m_iLeanEndTime) {
                if (m_iLeanDirection < 0) {
                    m_botCmd.buttons |= BUTTON_LEAN_LEFT;
                } else {
                    m_botCmd.buttons |= BUTTON_LEAN_RIGHT;
                }
            }
        } else {
            // Outside medium range, keep moving in open with clear 1.5s side switches.
            if (level.inttime >= m_iNextOrbitSwitchTime) {
                m_iOrbitDirection      = -m_iOrbitDirection;
                m_iNextOrbitSwitchTime = level.inttime + 1500;

                // Sync lean with the same 1.5s left/right dodge cycle.
                m_iLeanDirection = (m_iOrbitDirection < 0) ? -1 : 1;
                m_iLeanEndTime   = m_iNextOrbitSwitchTime;
            }

            Vector vLongSide = Vector(controlledEnt->orientation[1]) * (m_iOrbitDirection > 0 ? 1.0f : -1.0f);
            Vector vOpenFieldStrafe = controlledEnt->origin + vLongSide * 210.0f - vToEnemy * 24.0f;
            movement.MoveNear(vOpenFieldStrafe, 130.0f);

            if (m_iLeanDirection && level.inttime < m_iLeanEndTime) {
                if (m_iLeanDirection < 0) {
                    m_botCmd.buttons |= BUTTON_LEAN_LEFT;
                } else {
                    m_botCmd.buttons |= BUTTON_LEAN_RIGHT;
                }
            }
        }

        m_iAttackTime = level.inttime + 1000;
        return;
    }

    if (bCanSee && bCloseRange) {
        // In tight close combat, keep moving around the enemy aggressively
        if (level.inttime >= m_iNextOrbitSwitchTime) {
            if (G_Random(100) < 18 || controlledEnt->GetMoveResult() >= MOVERESULT_BLOCKED) {
                m_iOrbitDirection = -m_iOrbitDirection;
            }
            m_iNextOrbitSwitchTime = level.inttime + 520 + G_Random(420);
        }

        Vector vToEnemy = m_pEnemy->origin - controlledEnt->origin;
        if (vToEnemy.lengthSquared() > 0.001f) {
            VectorNormalizeFast(vToEnemy);
        }

        const float fCloseDistSq = (controlledEnt->origin - m_pEnemy->origin).lengthSquared();

        // Force circle-strafe in close range to avoid front-facing jitter.
        movement.ClearMove();
        m_botCmd.rightmove = (signed char)(m_iOrbitDirection > 0 ? 115 : -115);

        if (fCloseDistSq < Square(90.0f)) {
            m_botCmd.forwardmove = (signed char)-45;
        } else if (fCloseDistSq > Square(150.0f)) {
            m_botCmd.forwardmove = (signed char)35;
        } else {
            m_botCmd.forwardmove = 0;
        }

        if (controlledEnt->GetMoveResult() >= MOVERESULT_BLOCKED) {
            m_iOrbitDirection = -m_iOrbitDirection;
            m_botCmd.rightmove = (signed char)(m_iOrbitDirection > 0 ? 115 : -115);
        }

        Vector vSide = Vector(controlledEnt->orientation[1]) * (m_iOrbitDirection > 0 ? 1.0f : -1.0f);
        Vector vOrbitCenter = m_pEnemy->origin - vToEnemy * 44.0f;
        Vector vOrbitTarget = vOrbitCenter + vSide * 210.0f;

        movement.MoveNear(vOrbitTarget, 120.0f);

        if (fCloseDistSq < Square(116.0f)) {
            movement.AvoidPath(m_pEnemy->origin, 104.0f, vSide * 640.0f);
        }

        return;
    }

    fEnemyDistanceSquared = (controlledEnt->origin - m_vLastEnemyPos).lengthSquared();

    if ((!movement.MoveToBestAttractivePoint(5) && !movement.IsMoving())
        || (m_vOldEnemyPos != m_vLastEnemyPos && !movement.MoveDone()) || fEnemyDistanceSquared < fMinDistanceSquared) {
        if (!bMelee || !bCanSee) {
            if (fEnemyDistanceSquared < fMinDistanceSquared) {
                Vector vDir = controlledEnt->origin - m_vLastEnemyPos;
                VectorNormalizeFast(vDir);

                movement.AvoidPath(m_vLastEnemyPos, fMinDistance, Vector(controlledEnt->orientation[1]) * 512);
            } else {
                movement.MoveTo(m_vLastEnemyPos);
            }

            if (!bCanSee && movement.MoveDone()) {
                // Lost track of the enemy
                ClearEnemy();
                return;
            }
        } else {
            movement.MoveTo(m_vLastEnemyPos);
        }
    }

    if (movement.IsMoving()) {
        m_iAttackTime = level.inttime + 1000;
    }
}

/*
====================
Grenade state

Avoid any grenades
====================
*/
void BotController::InitState_Grenade(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Grenade;
    func->ThinkState     = &BotController::State_Grenade;
}

bool BotController::CheckCondition_Grenade(void)
{
    // FIXME: TODO
    return false;
}

void BotController::State_Grenade(void)
{
    // FIXME: TODO
}

/*
====================
Weapon state

Change weapon when necessary
====================
*/
void BotController::InitState_Weapon(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Weapon;
    func->BeginState     = &BotController::State_BeginWeapon;
}

bool BotController::CheckCondition_Weapon(void)
{
    return controlledEnt->GetActiveWeapon(WEAPON_MAIN)
        != controlledEnt->BestWeapon(NULL, false, WEAPON_CLASS_THROWABLE);
}

void BotController::State_BeginWeapon(void)
{
    Weapon *weap = controlledEnt->BestWeapon(NULL, false, WEAPON_CLASS_THROWABLE);

    if (weap == NULL) {
        SendCommand("safeholster 1");
        return;
    }

    SendCommand(va("use \"%s\"", weap->model.c_str()));
}

Weapon *BotController::FindWeaponWithAmmo()
{
    Weapon               *next;
    int                   n;
    int                   j;
    int                   bestrank;
    Weapon               *bestweapon;
    const Container<int>& inventory = controlledEnt->getInventory();

    n = inventory.NumObjects();

    // Search until we find the best weapon with ammo
    bestweapon = NULL;
    bestrank   = -999999;

    for (j = 1; j <= n; j++) {
        next = (Weapon *)G_GetEntity(inventory.ObjectAt(j));

        assert(next);
        if (!next->IsSubclassOfWeapon() || next->IsSubclassOfInventoryItem()) {
            continue;
        }

        if (next->GetWeaponClass() & WEAPON_CLASS_THROWABLE) {
            continue;
        }

        if (next->GetRank() < bestrank) {
            continue;
        }

        if (!next->HasAmmo(FIRE_PRIMARY)) {
            continue;
        }

        bestweapon = (Weapon *)next;
        bestrank   = bestweapon->GetRank();
    }

    return bestweapon;
}

Weapon *BotController::FindMeleeWeapon()
{
    Weapon               *next;
    int                   n;
    int                   j;
    int                   bestrank;
    Weapon               *bestweapon;
    const Container<int>& inventory = controlledEnt->getInventory();

    n = inventory.NumObjects();

    // Search until we find the best weapon with ammo
    bestweapon = NULL;
    bestrank   = -999999;

    for (j = 1; j <= n; j++) {
        next = (Weapon *)G_GetEntity(inventory.ObjectAt(j));

        assert(next);
        if (!next->IsSubclassOfWeapon() || next->IsSubclassOfInventoryItem()) {
            continue;
        }

        if (next->GetRank() < bestrank) {
            continue;
        }

        if (next->GetFireType(FIRE_SECONDARY) != FT_MELEE) {
            continue;
        }

        bestweapon = (Weapon *)next;
        bestrank   = bestweapon->GetRank();
    }

    return bestweapon;
}

void BotController::UseWeaponWithAmmo()
{
    Weapon *bestWeapon = FindWeaponWithAmmo();
    if (!bestWeapon) {
        //
        // If there is no weapon with ammo, fallback to a weapon that can melee
        //
        bestWeapon = FindMeleeWeapon();
    }

    if (!bestWeapon || bestWeapon == controlledEnt->GetActiveWeapon(WEAPON_MAIN)) {
        return;
    }

    controlledEnt->useWeapon(bestWeapon, WEAPON_MAIN);
}

void BotController::Spawned(void)
{
    ClearEnemy();
    m_iCuriousTime   = 0;
    m_botCmd.buttons = 0;

    // Reset human behavior on spawn
    m_fHeartRate           = 50.0f;
    m_iPanicLevel          = 0;
    m_fFatigueLevel        = 0.0f;
    m_bSuppressed          = false;
    m_iHesitationTime      = 0;
    m_fAimInaccuracy       = 0.0f;
    m_iLeanDirection       = 0;
    m_iLeanEndTime         = 0;
    m_iNextLeanDecisionTime = 0;
    m_iOrbitDirection      = (G_Random(100) < 50) ? -1 : 1;
    m_iNextOrbitSwitchTime = 0;
    m_iHoldPositionUntilTime = 0;
    m_iNextHoldCheckTime = 0;
    m_iCoverPeekState      = 0;
    m_iCoverPeekSwitchTime = 0;
}

void BotController::Think()
{
    usercmd_t  ucmd;
    usereyes_t eyeinfo;

    UpdateBotStates();
    GetUsercmd(&ucmd);
    GetEyeInfo(&eyeinfo);

    G_ClientThink(controlledEnt->edict, &ucmd, &eyeinfo);
}

void BotController::Killed(const Event& ev)
{
    Entity *attacker;

    // send the respawn buttons
    if (!(m_botCmd.buttons & BUTTON_ATTACKLEFT)) {
        m_botCmd.buttons |= BUTTON_ATTACKLEFT;
    } else {
        m_botCmd.buttons &= ~BUTTON_ATTACKLEFT;
    }

    m_botEyes.ofs[0]    = 0;
    m_botEyes.ofs[1]    = 0;
    m_botEyes.ofs[2]    = 0;
    m_botEyes.angles[0] = 0;
    m_botEyes.angles[1] = 0;

    attacker = ev.GetEntity(1);

    if (attacker && rand() % 5 == 0) {
        // 1/5 chance to go back to the attacker position
        m_vLastDeathPos = attacker->origin;
    } else {
        m_vLastDeathPos = vec_zero;
    }

    // Choose a new random primary weapon
    Event event(EV_Player_PrimaryDMWeapon);
    event.AddString("auto");

    controlledEnt->ProcessEvent(event);

    //
    // This is useful to change nationality in Spearhead and Breakthrough
    // this allows the AI to use more weapons
    //
    Info_SetValueForKey(controlledEnt->client->pers.userinfo, "dm_playermodel", G_GetRandomAlliedPlayerModel());
    Info_SetValueForKey(controlledEnt->client->pers.userinfo, "dm_playergermanmodel", G_GetRandomGermanPlayerModel());

    G_ClientUserinfoChanged(controlledEnt->edict, controlledEnt->client->pers.userinfo);
}

void BotController::GotKill(const Event& ev)
{
    ClearEnemy();
    m_iCuriousTime = 0;

    if (g_bot_instamsg_chance->integer && level.inttime >= m_iNextTauntTime && (rand() % g_bot_instamsg_chance->integer) == 0) {
        //
        // Randomly play a taunt
        //
        Event event("dmmessage");

        event.AddInteger(0);

        if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
            event.AddString("*5" + str(1 + (rand() % 8)));
        } else {
            event.AddString("*4" + str(1 + (rand() % 9)));
        }

        controlledEnt->ProcessEvent(event);

        m_iNextTauntTime = level.inttime + g_bot_instamsg_delay->integer;
    }
}

void BotController::EventDamaged(const Event& ev)
{
    Entity   *attacker;
    Sentient *attackerSent;

    if (!controlledEnt || controlledEnt->deadflag != DEAD_NO) {
        return;
    }

    attacker = ev.GetEntity(1);

    if (!attacker || attacker == controlledEnt || !attacker->IsSubclassOfSentient()) {
        return;
    }

    attackerSent = static_cast<Sentient *>(attacker);
    if (!IsValidEnemy(attackerSent)) {
        return;
    }

    m_pEnemy        = attackerSent;
    m_iEnemyEyesTag = -1;
    m_vLastEnemyPos = attackerSent->origin;
    m_iLastSeenTime = level.inttime;
    m_iAttackTime   = level.inttime + 3500;

    Vector vToAttacker = attackerSent->origin - controlledEnt->origin;
    Vector vForward    = Vector(controlledEnt->orientation[0]);

    vToAttacker.z = 0;
    vForward.z    = 0;

    if (vToAttacker.lengthSquared() <= Square(4.0f) || vForward.lengthSquared() <= Square(0.01f)) {
        return;
    }

    VectorNormalize2D(vToAttacker);
    VectorNormalize2D(vForward);

    const float frontDot = DotProduct(vForward, vToAttacker);

    if (frontDot < -0.15f) {
        Vector vTargetAngles = (attackerSent->origin - controlledEnt->EyePosition()).toAngles();

        vTargetAngles[PITCH] = Q_clamp_float(vTargetAngles[PITCH], -40.0f, 40.0f);
        rotation.SnapToAngles(vTargetAngles);
        m_botEyes.angles[0] = vTargetAngles[0];
        m_botEyes.angles[1] = vTargetAngles[1];

        m_iHesitationTime = level.inttime;
        m_iPanicLevel = Q_max(0, m_iPanicLevel - 15);
    }
}

void BotController::EventStuffText(const str& text)
{
    SendCommand(text);
}

void BotController::setControlledEntity(Player *player)
{
    controlledEnt = player;
    movement.SetControlledEntity(player);
    rotation.SetControlledEntity(player);

    delegateHandle_gotKill =
        player->delegate_gotKill.Add(std::bind(&BotController::GotKill, this, std::placeholders::_1));
    delegateHandle_killed = player->delegate_killed.Add(std::bind(&BotController::Killed, this, std::placeholders::_1));
    delegateHandle_damage = player->delegate_damage.Add(std::bind(&BotController::EventDamaged, this, std::placeholders::_1));
    delegateHandle_stufftext =
        player->delegate_stufftext.Add(std::bind(&BotController::EventStuffText, this, std::placeholders::_1));
    delegateHandle_spawned = player->delegate_spawned.Add(std::bind(&BotController::Spawned, this));
}

Player *BotController::getControlledEntity() const
{
    return controlledEnt;
}

BotController *BotControllerManager::createController(Player *player)
{
    BotController *controller = new BotController();
    controller->setControlledEntity(player);

    controllers.AddObject(controller);

    return controller;
}

void BotControllerManager::removeController(BotController *controller)
{
    controllers.RemoveObject(controller);
    delete controller;
}

BotController *BotControllerManager::findController(Entity *ent)
{
    int i;

    for (i = 1; i <= controllers.NumObjects(); i++) {
        BotController *controller = controllers.ObjectAt(i);
        if (controller->getControlledEntity() == ent) {
            return controller;
        }
    }

    return nullptr;
}

const Container<BotController *>& BotControllerManager::getControllers() const
{
    return controllers;
}

BotControllerManager::~BotControllerManager()
{
    Cleanup();
}

void BotControllerManager::Init()
{
    BotController::Init();
}

void BotControllerManager::Cleanup()
{
    int i;

    BotController::Init();

    for (i = 1; i <= controllers.NumObjects(); i++) {
        BotController *controller = controllers.ObjectAt(i);
        delete controller;
    }

    controllers.FreeObjectList();
}

void BotControllerManager::ThinkControllers()
{
    int i;

    // Delete controllers that don't have associated player entity
    // This cannot happen unless some mods remove them
    for (i = controllers.NumObjects(); i > 0; i--) {
        BotController *controller = controllers.ObjectAt(i);
        if (!controller->getControlledEntity()) {
            gi.DPrintf(
                "Bot %d has no associated player entity. This shouldn't happen unless the entity has been removed by a "
                "script. The controller will be removed, please fix.\n",
                i
            );

            // Remove the controller, it will be recreated later to match `sv_numbots`
            delete controller;
            controllers.RemoveObjectAt(i);
        }
    }

    for (i = 1; i <= controllers.NumObjects(); i++) {
        BotController *controller = controllers.ObjectAt(i);
        controller->Think();
    }
}

/*
====================
UpdateStressLevel

Increase stress based on enemy presence and damage
====================
*/
void BotController::UpdateStressLevel(void)
{
    float stressIncrease = 0.0f;

    // Stress increases when under fire
    if (m_bSuppressed) {
        stressIncrease += 15.0f;
    }

    // Stress increases with lower health
    if (controlledEnt->health < controlledEnt->max_health * 0.5f) {
        stressIncrease += 10.0f;
    }

    // Stress increases when enemy is very close
    if (m_pEnemy) {
        float distSq = (m_pEnemy->origin - controlledEnt->origin).lengthSquared();
        if (distSq < Square(512)) {  // Close range
            stressIncrease += 8.0f;
        } else if (distSq < Square(1024)) {  // Medium range
            stressIncrease += 5.0f;
        }
    }

    // Gradually increase stress
    m_fHeartRate = Q_min(200.0f, m_fHeartRate + stressIncrease * level.frametime);

    // Gradually recover when safe
    if (!m_pEnemy && !m_bSuppressed) {
        m_fHeartRate = Q_max(50.0f, m_fHeartRate - 15.0f * level.frametime);
    }

    m_iLastStressEvent = level.inttime;
}

/*
====================
UpdatePanicLevel

Panic increases with stress and low health
====================
*/
void BotController::UpdatePanicLevel(void)
{
    // Panic is based on stress level
    float stressPercent = (m_fHeartRate - 50.0f) / 150.0f;  // Normalize to 0-1
    float healthPercent = controlledEnt->health / (float)controlledEnt->max_health;

    // Panic can be triggered by very low health or high stress
    if (healthPercent < 0.15f || stressPercent > 0.9f) {
        m_iPanicLevel = Q_min(100, m_iPanicLevel + 15);
    } else if (healthPercent < 0.35f) {
        m_iPanicLevel = Q_min(100, m_iPanicLevel + 8);
    } else {
        // Recover from panic
        m_iPanicLevel = Q_max(0, m_iPanicLevel - 12);
    }
}

/*
====================
UpdateFatigue

Fatigue increases when sprinting/running
====================
*/
void BotController::UpdateFatigue(void)
{
    float velocity = controlledEnt->velocity.length();
    float runSpeed = sv_runspeed->value;

    // Fatigue increases when running
    if (velocity > runSpeed * 0.5f) {
        m_fFatigueLevel = Q_min(1.0f, m_fFatigueLevel + 0.5f * level.frametime);
        m_iLastSprintTime = level.inttime;
    } else {
        // Recover from fatigue when walking/standing
        m_fFatigueLevel = Q_max(0.0f, m_fFatigueLevel - 0.3f * level.frametime);
    }

    // Fatigue slows movement and reduces effectiveness
    if (m_fFatigueLevel > 0.7f) {
        // Bot is very tired - reduce movement effectiveness
        if (controlledEnt->velocity.length() > 0) {
            controlledEnt->velocity *= (1.0f - m_fFatigueLevel * 0.3f);
        }
    }
}

/*
====================
ApplyAimInaccuracy

Add randomness to aim based on stress, fatigue, and distance
====================
*/
void BotController::ApplyAimInaccuracy(Vector& targetPos)
{
    float inaccuracy = 0.0f;
    float distanceMult = 1.0f;

    // Stress reduces accuracy
    inaccuracy += (m_fHeartRate - 50.0f) / 150.0f * 40.0f;  // Up to 40 units at max stress

    // Fatigue reduces accuracy
    inaccuracy += m_fFatigueLevel * 30.0f;  // Up to 30 units

    // Panic significantly reduces accuracy
    inaccuracy += (m_iPanicLevel / 100.0f) * 40.0f;  // Up to 40 units

    // Bots are steadier at close range
    if (m_pEnemy) {
        float distSq = (m_pEnemy->origin - controlledEnt->origin).lengthSquared();
        if (distSq < Square(256.0f)) {
            distanceMult = 0.45f;
        } else if (distSq < Square(512.0f)) {
            distanceMult = 0.65f;
        } else if (distSq < Square(1024.0f)) {
            distanceMult = 0.85f;
        }
    }

    inaccuracy *= distanceMult;

    // Add random offset based on calculated inaccuracy
    if (inaccuracy > 0.0f) {
        Vector offset(
            G_CRandom(inaccuracy),
            G_CRandom(inaccuracy),
            G_CRandom(inaccuracy * 0.5f)  // Less vertical inaccuracy
        );
        targetPos += offset;
    }

    m_fAimInaccuracy = Q_min(1.0f, inaccuracy / 150.0f);
    m_iLastAimAdjustTime = level.inttime;
}

/*
====================
CheckForCover

Bots search for cover when under heavy fire
====================
*/
void BotController::CheckForCover(void)
{
    if (level.inttime < m_iLastCoverCheckTime + 500) {
        return;  // Don't check too frequently
    }

    // If enemy is very close, fight instead of always trying to retreat
    if (m_pEnemy) {
        float distSq = (m_pEnemy->origin - controlledEnt->origin).lengthSquared();
        if (distSq < Square(384.0f)) {
            m_iLastCoverCheckTime = level.inttime;
            return;
        }
    }

    // If bot is heavily suppressed or low health, seek cover
    if (m_bSuppressed || controlledEnt->health < controlledEnt->max_health * 0.4f) {
        // Perform a simple cover-seeking by moving away from last enemy position
        if (m_vLastEnemyPos != vec_zero) {
            Vector awayDir = controlledEnt->origin - m_vLastEnemyPos;
            awayDir.normalize();

            // Look for cover in the away direction
            m_vLastCoverPos = controlledEnt->origin + awayDir * 512.0f;
            movement.MoveTo(m_vLastCoverPos);
        }
    }

    m_iLastCoverCheckTime = level.inttime;
}

/*
====================
UpdateSuppression

Simulate suppressive fire effects
====================
*/
void BotController::UpdateSuppression(void)
{
    bool bRecentlyHit = level.inttime <= controlledEnt->m_iLastHitTime + 900;

    // Check if bot is being shot at (simple heuristic)
    if (m_pEnemy && level.inttime <= controlledEnt->m_iLastHitTime + 1200) {
        m_bSuppressed = true;
        m_iSuppressionEndTime = level.inttime + G_Random(800) + 300;  // 0.3-1.1 seconds
    }

    // Suppression wears off
    if (level.inttime > m_iSuppressionEndTime && m_iSuppressionEndTime > 0) {
        m_bSuppressed = false;
    }

    // Suppressed bots are less effective
    if (m_bSuppressed) {
        // Reduce aim accuracy further
        m_fAimInaccuracy = Q_min(1.0f, m_fAimInaccuracy + 0.2f);

        // Potential hesitation
        if (G_Random(100) < (bRecentlyHit ? 5 : 15)) {
            m_iHesitationTime = level.inttime + G_Random(bRecentlyHit ? 120 : 220) + (bRecentlyHit ? 40 : 80);
        }
        gi.DPrintf("[BOTDBG] %s suppressed, aimInacc=%.2f hesitationUntil=%d\n", controlledEnt->client->pers.netname, m_fAimInaccuracy, m_iHesitationTime);
    }
}

/*
====================
UpdateReloadBehavior

More realistic reload behavior with delays
====================
*/
void BotController::UpdateReloadBehavior(void)
{
    Weapon *weapon = controlledEnt->GetActiveWeapon(WEAPON_MAIN);

    if (!weapon) {
        m_bReloading = false;
        // Debug
        gi.DPrintf("[BOTDBG] %s UpdateReloadBehavior: no weapon\n", controlledEnt->client->pers.netname);
        return;
    }

    // Check if weapon needs reload
    if (weapon->HasAmmoInClip(FIRE_PRIMARY) == 0 && weapon->HasAmmo(FIRE_PRIMARY)) {
        if (!m_bReloading) {
            m_bReloading = true;
            m_iReloadStartTime = level.inttime;

            // Add some hesitation before reloading (humans don't reload instantly)
            m_iReloadStartTime += G_Random(500) + 100;  // 0.1-0.6 seconds delay
            gi.DPrintf("[BOTDBG] %s starts reload at %d (will begin after delay)\n", controlledEnt->client->pers.netname, m_iReloadStartTime);
        }
    } else {
        m_bReloading = false;
        gi.DPrintf("[BOTDBG] %s not reloading\n", controlledEnt->client->pers.netname);
    }

    // During reload, don't shoot
    if (m_bReloading) {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    }
}
