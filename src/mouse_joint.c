// SPDX-FileCopyrightText: 2023 Erin Catto
// SPDX-License-Identifier: MIT

#include "body.h"
#include "joint.h"
#include "solver_data.h"
#include "world.h"

// p = attached point, m = mouse point
// C = p - m
// Cdot = v
//      = v + cross(w, r)
// J = [I r_skew]
// Identity used:
// w k % (rx i + ry mouse) = w * (-ry i + rx mouse)

void b2MouseJoint_SetTarget(b2JointId jointId, b2Vec2 target)
{
	b2World* world = b2GetWorldFromIndex(jointId.world);
	assert(world->locked == false);
	if (world->locked)
	{
		return;
	}

	assert(0 <= jointId.index && jointId.index < world->jointPool.capacity);

	b2Joint* base = world->joints + jointId.index;
	assert(base->object.index == base->object.next);
	assert(base->object.revision == jointId.revision);
	assert(base->type == b2_mouseJoint);
	base->mouseJoint.targetA = target;
}

void b2InitializeMouse(b2Joint* base, b2SolverContext* context)
{
	assert(base->type == b2_mouseJoint);

	b2MouseJoint* joint = &base->mouseJoint;
	int32_t indexB = base->islandIndexB;

	joint->localCenterB = context->bodyData[indexB].localCenter;
	joint->invMassB = context->bodyData[indexB].invMass;
	joint->invIB = context->bodyData[indexB].invI;

	b2Vec2 cB = context->positions[indexB].c;
	float aB = context->positions[indexB].a;
	b2Vec2 vB = context->velocities[indexB].v;
	float wB = context->velocities[indexB].w;

	b2Rot qB = b2MakeRot(aB);

	float d = joint->damping;
	float k = joint->stiffness;

	// magic formulas
	// gamma has units of inverse mass.
	// beta has units of inverse time.
	float h = context->step->dt;
	joint->gamma = h * (d + h * k);
	if (joint->gamma != 0.0f)
	{
		joint->gamma = 1.0f / joint->gamma;
	}
	joint->beta = h * k * joint->gamma;

	// Compute the effective mass matrix.
	joint->rB = b2RotateVector(qB, b2Sub(base->localAnchorB, joint->localCenterB));

	// K    = [(1/m1 + 1/m2) * eye(2) - skew(r1) * invI1 * skew(r1) - skew(r2) * invI2 * skew(r2)]
	//      = [1/m1+1/m2     0    ] + invI1 * [r1.y*r1.y -r1.x*r1.y] + invI2 * [r1.y*r1.y -r1.x*r1.y]
	//        [    0     1/m1+1/m2]           [-r1.x*r1.y r1.x*r1.x]           [-r1.x*r1.y r1.x*r1.x]
	b2Mat22 K;
	K.cx.x = joint->invMassB + joint->invIB * joint->rB.y * joint->rB.y + joint->gamma;
	K.cx.y = -joint->invIB * joint->rB.x * joint->rB.y;
	K.cy.x = K.cx.y;
	K.cy.y = joint->invMassB + joint->invIB * joint->rB.x * joint->rB.x + joint->gamma;

	joint->mass = b2GetInverse22(K);

	joint->C = b2Add(cB, b2Sub(joint->rB, joint->targetA));
	joint->C = b2MulSV(joint->beta, joint->C);

	// Cheat with some damping
	wB *= B2_MAX(0.0f, 1.0f - 0.02f * (60.0f * h));

	if (context->step->warmStarting)
	{
		joint->impulse = b2MulSV(context->step->dtRatio, joint->impulse);
		vB = b2MulAdd(vB, joint->invMassB, joint->impulse);
		wB += joint->invIB * b2Cross(joint->rB, joint->impulse);
	}
	else
	{
		joint->impulse = b2Vec2_zero;
	}

	context->velocities[indexB].v = vB;
	context->velocities[indexB].w = wB;
}

void b2SolveMouseVelocity(b2Joint* base, b2SolverContext* data)
{
	b2MouseJoint* joint = &base->mouseJoint;
	int32_t indexB = base->islandIndexB;

	b2Vec2 vB = data->velocities[indexB].v;
	float wB = data->velocities[indexB].w;

	// Cdot = v + cross(w, r)
	b2Vec2 Cdot = b2Add(vB, b2CrossSV(wB, joint->rB));
	b2Vec2 SoftCdot = b2Add(Cdot, b2MulAdd(joint->C, joint->gamma, joint->impulse));
	b2Vec2 impulse = b2Neg(b2MulMV(joint->mass, SoftCdot));

	b2Vec2 oldImpulse = joint->impulse;
	joint->impulse = b2Add(joint->impulse, impulse);
	float maxImpulse = data->step->dt * joint->maxForce;
	if (b2LengthSquared(joint->impulse) > maxImpulse * maxImpulse)
	{
		joint->impulse = b2MulSV(maxImpulse / b2Length(joint->impulse), joint->impulse);
	}
	impulse = b2Sub(joint->impulse, oldImpulse);

	vB = b2MulAdd(vB, joint->invMassB, impulse);
	wB += joint->invIB * b2Cross(joint->rB, impulse);

	data->velocities[indexB].v = vB;
	data->velocities[indexB].w = wB;
}