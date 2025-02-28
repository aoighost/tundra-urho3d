// For conditions of distribution and use, see copyright notice in LICENSE

#include "StableHeaders.h"
#include "SceneInteract.h"

#include "GraphicsWorld.h"
#include "Framework.h"
#include "Placeable.h"
#include "Entity.h"
#include "Scene/Scene.h"
#include "LoggingFunctions.h"
#include "UrhoRenderer.h"
#include "InputAPI.h"
#include "InputContext.h"

#include <Math/float3.h>
#include <Math/MathFunc.h>

#include <Urho3D/Core/Profiler.h>
#include <Urho3D/Core/StringUtils.h>
#include <Urho3D/UI/UI.h>

namespace Tundra
{

SceneInteract::SceneInteract(Framework* owner) :
    IModule("SceneInteract", owner),
    lastX(-1),
    lastY(-1),
    frameRaycasted(false),
    itemUnderMouse(false)
{
}

SceneInteract::~SceneInteract()
{
}

void SceneInteract::Initialize()
{
    inputContext = framework->Input()->RegisterInputContext("SceneInteract", 100);
    inputContext->MouseEventReceived.Connect(this, &SceneInteract::HandleMouseEvent);
}

void SceneInteract::Uninitialize()
{
    inputContext.Reset();
}

void SceneInteract::Update(float /*frameTime*/)
{
    if (!framework->IsHeadless())
    {
        URHO3D_PROFILE(SceneInteract_Update);

        ExecuteRaycast();
        if (lastHitEntity)
            lastHitEntity->Exec(EntityAction::Local, "MouseHover");
        
        frameRaycasted = false;
    }
}

RayQueryResult* SceneInteract::CurrentMouseRaycastResult() const
{
    return &lastRaycast;
}

float3 SceneInteract::RaycastClosestIntersect(const float3 &from, const float3 &to, unsigned layerMask, float maxDistance) const
{
    Vector<float3> toVec;
    toVec.Push(to);
    return RaycastClosestIntersect(from, toVec, layerMask, maxDistance);
}

float3 SceneInteract::RaycastClosestIntersect(const float3 &from, const Vector<float3> &to, unsigned layerMask, float maxDistance) const
{
    float3 intersection = float3::nan;
    Scene* scene = framework->Module<UrhoRenderer>()->MainCameraScene();
    GraphicsWorld* world = scene ? scene->Subsystem<GraphicsWorld>().Get() : nullptr;
    if (!world)
        return intersection;

    Ray ray;
    ray.pos = from;

    float closest = FLT_MAX;
    for (int i=0, len=to.Size(); i<len; ++i)
    {
        ray.dir = to[i].Sub(from).Normalized();
        
        // We use Raycast as the closest hit entity is enough for us.
        RayQueryResult result = world->Raycast(ray, layerMask, maxDistance);
        if (result.entity && result.t < closest)
        {
            closest = result.t;
            intersection = result.pos;
        }
    }
    return intersection;
}

float3 SceneInteract::RaycastFurthestIntersect(const float3 &from, const float3 &to, unsigned layerMask) const
{
    Vector<float3> toVec;
    toVec.Push(to);
    return RaycastFurthestIntersect(from, toVec, layerMask);
}

float3 SceneInteract::RaycastFurthestIntersect(const float3 &from, const Vector<float3> &to, unsigned layerMask) const
{
    float3 intersection = float3::nan;
    Scene* scene = framework->Module<UrhoRenderer>()->MainCameraScene();
    GraphicsWorld* world = scene ? scene->Subsystem<GraphicsWorld>().Get() : nullptr;
    if (!world)
        return intersection;

    Ray ray;
    ray.pos = from;

    float furthest = 0.0f;
    for (int i=0, len=to.Size(); i<len; ++i)
    {
        ray.dir = to[i].Sub(from).Normalized();

        // We use raycast all as there might be multiple entities in between 'from' and 'to'
        // and this function should return the closest hit to the 'to' target.
        float maxDistance = from.Distance(to[i]);
        RayQueryResultVector results = world->RaycastAll(ray, layerMask, maxDistance);
        if (!results.Empty())
        {
            // Last result is picked as the results are already ordered by distance.
            RayQueryResult& result = results.Back();
            if (result.t > furthest)
            {
                furthest = result.t;
                intersection = result.pos;
            }
        }
    }
    return intersection;
}

RayQueryResult* SceneInteract::ExecuteRaycast()
{
    // Return the cached result if already executed this frame.
    if (frameRaycasted)
        return &lastRaycast;
    frameRaycasted = true;

    Scene* scene = framework->Module<UrhoRenderer>()->MainCameraScene();
    GraphicsWorld* world = scene ? scene->Subsystem<GraphicsWorld>().Get() : nullptr;
    if (!world)
        return 0;

    lastRaycast = world->Raycast(lastX, lastY);
    if (!lastRaycast.entity || itemUnderMouse)
    {
        if (lastHitEntity)
            lastHitEntity->Exec(EntityAction::Local, "MouseHoverOut");
        lastHitEntity.Reset();
        return &lastRaycast;
    }

    EntityWeakPtr lastEntity = lastHitEntity;
    EntityWeakPtr entity(lastRaycast.entity);
    if (entity != lastEntity)
    {
        if (lastEntity)
            lastEntity->Exec(EntityAction::Local, "MouseHoverOut");

        if (entity)
            entity->Exec(EntityAction::Local, "MouseHoverIn");

        lastHitEntity = entity;
    }

    return &lastRaycast;
}

void SceneInteract::HandleMouseEvent(MouseEvent* e)
{
    // Invalidate cached raycast if mouse coordinates have changed
    if (frameRaycasted)
        if (lastX != e->x || lastY != e->y)
            frameRaycasted = false;

    lastX = e->x;
    lastY = e->y;
    itemUnderMouse = GetSubsystem<Urho3D::UI>()->GetElementAt(lastX, lastY, true) != nullptr;

    RayQueryResult *raycastResult = ExecuteRaycast();

    Entity *hitEntity = lastHitEntity;
    if (!hitEntity || !raycastResult)
        return;

    StringVector actionParams;

    if (lastHitEntity)
    {
        if(e->Type() == MouseEvent::MouseMove)
        {
            EntityMouseMove.Emit(hitEntity, e->otherButtons, raycastResult);
        }
        else if (e->Type() == MouseEvent::MouseScroll)
        {
            actionParams.Push(String(e->relativeZ));
            actionParams.Push(Urho3D::ToString("%f,%f,%f", raycastResult->pos.x, raycastResult->pos.y, raycastResult->pos.z));
            hitEntity->Exec(EntityAction::Local, "MouseScroll", actionParams);
            EntityMouseScroll.Emit(hitEntity, e->relativeZ, raycastResult);
        }
        else if (e->Type() == MouseEvent::MousePressed)
        {
            // Execute local entity action with signature:
            // Action name: "MousePress"
            // String parameters: (int)mouseButton, (float,float,float)"x,y,z"
            actionParams.Push(String((int)e->button));
            actionParams.Push(Urho3D::ToString("%f,%f,%f", raycastResult->pos.x, raycastResult->pos.y, raycastResult->pos.z));
            hitEntity->Exec(EntityAction::Local, "MousePress", actionParams);
            EntityClicked.Emit(hitEntity, (int)e->button, raycastResult);
        }
        else if (e->Type() == MouseEvent::MouseReleased)
        {
            // Execute local entity action with signature:
            // Action name: "MouseRelease"
            // String parameters: (int)mouseButton, (float,float,float)"x,y,z", (int)"submesh index"
            actionParams.Push(String((int)e->button));
            actionParams.Push(Urho3D::ToString("%f,%f,%f", raycastResult->pos.x, raycastResult->pos.y, raycastResult->pos.z));
            hitEntity->Exec(EntityAction::Local, "MouseRelease", actionParams);
            EntityClickReleased.Emit(hitEntity, (int)e->button, raycastResult);
        }
    }
}

}

extern "C"
{

DLLEXPORT void TundraPluginMain(Tundra::Framework *fw)
{
    fw->RegisterModule(new Tundra::SceneInteract(fw));
}

}
