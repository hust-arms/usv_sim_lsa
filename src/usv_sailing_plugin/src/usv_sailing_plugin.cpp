#include <algorithm>
#include <string>

#include <gazebo/common/Assert.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/sensors/SensorManager.hh>
#include <gazebo/transport/transport.hh>
#include <usv_sailing_plugin/usv_sailing_plugin.h>

#include <ros/publisher.h>
#include <sensor_msgs/JointState.h>

using namespace gazebo;

GZ_REGISTER_MODEL_PLUGIN (USV_Sailing_Plugin)

/////////////////////////////////////////////////
USV_Sailing_Plugin::USV_Sailing_Plugin () :
		cla (1.0), cda (0.01), cma (0.01), rho (1.2041)
{
	ROS_INFO ("------------------------------USV_Sailing_Plugin OBJECT CREATED!!!!");
	this->cp = math::Vector3 (0, 0, 0);
	this->forward = math::Vector3 (1, 0, 0);
	this->upward = math::Vector3 (0, 0, 1);
	this->area = 1.0;
	this->alpha0 = 0.0;

	// 90 deg stall
	this->alphaStall = 0.5 * M_PI;
	this->claStall = 0.0;

	this->cdaStall = 1.0;
	this->cmaStall = 0.0;
	this->wind = 0.0;
}

/////////////////////////////////////////////////
void
USV_Sailing_Plugin::Load (physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
	ROS_INFO ("------------------------------USV_Sailing_Plugin loaded!!!!");

	GZ_ASSERT (_model, "USV_Sailing_Plugin _model pointer is NULL");
	GZ_ASSERT (_sdf, "USV_Sailing_Plugin _sdf pointer is NULL");
	this->model = _model;
	this->modelName = _model->GetName ();
	this->sdf = _sdf;
	rosnode_ = ros::NodeHandle (modelName);

	this->world = this->model->GetWorld ();
	GZ_ASSERT (this->world, "USV_Sailing_Plugin world pointer is NULL");

	this->physics = this->world->GetPhysicsEngine ();
	GZ_ASSERT (this->physics, "USV_Sailing_Plugin physics pointer is NULL");

	GZ_ASSERT (_sdf, "USV_Sailing_Plugin _sdf pointer is NULL");

	if (_sdf->HasElement ("a0"))
		this->alpha0 = _sdf->Get<double> ("a0");

	if (_sdf->HasElement ("cla"))
		this->cla = _sdf->Get<double> ("cla");

	if (_sdf->HasElement ("cda"))
		this->cda = _sdf->Get<double> ("cda");

	if (_sdf->HasElement ("cma"))
		this->cma = _sdf->Get<double> ("cma");

	if (_sdf->HasElement ("alpha_stall"))
		this->alphaStall = _sdf->Get<double> ("alpha_stall");

	if (_sdf->HasElement ("cla_stall"))
		this->claStall = _sdf->Get<double> ("cla_stall");

	if (_sdf->HasElement ("cda_stall"))
		this->cdaStall = _sdf->Get<double> ("cda_stall");

	if (_sdf->HasElement ("cma_stall"))
		this->cmaStall = _sdf->Get<double> ("cma_stall");

	if (_sdf->HasElement ("cp"))
		this->cp = _sdf->Get < math::Vector3 > ("cp");

	// blade forward (-drag) direction in link frame
	if (_sdf->HasElement ("forward"))
		this->forward = _sdf->Get < math::Vector3 > ("forward");

	// blade upward (+lift) direction in link frame
	if (_sdf->HasElement ("upward"))
		this->upward = _sdf->Get < math::Vector3 > ("upward");

	if (_sdf->HasElement ("area"))
		this->area = _sdf->Get<double> ("area");

	if (_sdf->HasElement ("air_density"))
		this->rho = _sdf->Get<double> ("air_density");

	if (_sdf->HasElement ("link_name"))
	{
		sdf::ElementPtr elem = _sdf->GetElement ("link_name");
		this->linkName = elem->Get<std::string> ();
		this->link = this->model->GetLink (this->linkName);
	}

	if (_sdf->HasElement ("link_type"))
	{
		sdf::ElementPtr elem = _sdf->GetElement ("link_type");
		this->linkType = elem->Get<std::string> ();
	}
	if (_sdf->HasElement ("joint_name"))
	{
		sdf::ElementPtr elem = _sdf->GetElement ("joint_name");
		this->jointName = elem->Get<std::string> ();
		this->joint = this->model->GetJoint (this->jointName);
		std::cerr << "Joint name: [" << this->jointName << "\n";
		std::cerr << "Joint: [" << this->joint->GetName () << "\n";
	}
	if (_sdf->HasElement ("fluidVelocity"))
	{
		sdf::ElementPtr elem = _sdf->GetElement ("fluidVelocity");
		this->fluidVelocity = elem->Get<std::string> ();
		std::cerr << "fluidVelocity: [" << this->fluidVelocity << "\n";
	}

	waterCurrent = math::Vector3 (0, 0, 0);
	float wind_value_x;
	float wind_value_y;
	float wind_value_z;
	running = false;
	if (fluidVelocity.compare ("global") == 0)
	{
		if (rosnode_.getParam ("/uwsim/wind/x", wind_value_x) & rosnode_.getParam ("/uwsim/wind/y", wind_value_y))
		{
			this->wind = math::Vector3 (wind_value_x, wind_value_y, 0);
		}
		else
		{
			ROS_INFO ("Sail plugin error: Cant find value of /uwsim/wind in param server");
		}
	}
	else if (fluidVelocity.compare ("local") == 0)
	{
		if (this->linkType.compare ("sail") == 0)
		{
			std::cerr << "\n initializing service client";
			velocity_serviceClient_ = rosnode_.serviceClient < usv_wind_current::GetSpeed > ("/windCurrent");
			std::cerr << " ... done";
			running = true;
			the_thread = std::thread (&USV_Sailing_Plugin::WindThreadLoop, this);
		}
		else
		{
			std::cerr << "\n initializing service client";
			velocity_serviceClient_ = rosnode_.serviceClient < usv_water_current::GetSpeed > ("/waterCurrent");
			std::cerr << " ... done";
			running = true;
			the_thread = std::thread (&USV_Sailing_Plugin::WaterThreadLoop, this);
		}
	}
}

/////////////////////////////////////////////////
void
USV_Sailing_Plugin::Init ()
{
	std::cerr << "\n ----------- USV_Sailing_Plugin::init: type: " << this->linkType << " linkName: " << this->linkName;
	current_subscriber_ = rosnode_.subscribe ("/gazebo/current", 1, &USV_Sailing_Plugin::ReadWaterCurrent, this);
	this->updateConnection = event::Events::ConnectWorldUpdateBegin (boost::bind (&USV_Sailing_Plugin::OnUpdate, this));

	std::cerr << "\n compare to sail: " << this->linkType.compare ("sail");
	if (this->linkType.compare ("sail") == 0)
	{
		std::string topic = "/" + this->model->GetName () + "/angleLimits";
		this->angleLimits_subscriber = rosnode_.subscribe (topic, 1, &USV_Sailing_Plugin::ropeSimulator, this);
	}
}

void
USV_Sailing_Plugin::OnUpdate ()
{
	if (this->linkType.compare ("rudder") == 0)
		this->OnUpdateRudder ();
	else if (this->linkType.compare ("keel") == 0)
		this->OnUpdateKeel ();
	else if (this->linkType.compare ("sail") == 0)
		this->OnUpdateSail ();
}

/////////////////////////////////////////////////
void
USV_Sailing_Plugin::OnUpdateRudder ()
{
	// get linear velocity at cp in inertial frame
	math::Vector3 vel = this->link->GetWorldLinearVel (this->cp) - waterCurrent;

	if (vel.GetLength () <= 0.01)
		return;

	// pose of body
	math::Pose pose = this->link->GetWorldPose ();

	// rotate forward and upward vectors into inertial frame
	math::Vector3 forwardI = pose.rot.RotateVector (this->forward);
	math::Vector3 upwardI = pose.rot.RotateVector (this->upward);
	//std::cerr<<"\n pose: "<<pose<<" forwardI: "<<forwardI<<" upwardI: "<<upwardI;

	// ldNormal vector to lift-drag-plane described in inertial frame
	math::Vector3 ldNormal = forwardI.Cross (upwardI).Normalize ();

	// check sweep (angle between vel and lift-drag-plane)
	double sinSweepAngle = ldNormal.Dot (vel) / vel.GetLength ();

	// get cos from trig identity
	double cosSweepAngle2 = (1.0 - sinSweepAngle * sinSweepAngle);
	this->sweep = asin (sinSweepAngle);

	// truncate sweep to within +/-90 deg
	while (fabs (this->sweep) > 0.5 * M_PI)
		this->sweep = this->sweep > 0 ? this->sweep - M_PI : this->sweep + M_PI;

	// angle of attack is the angle between 
	// vel projected into lift-drag plane
	//  and
	// forward vector
	//
	// projected = ldNormal Xcross ( vector Xcross ldNormal)
	//
	// so,
	// velocity in lift-drag plane (expressed in inertial frame) is:
	math::Vector3 velInLDPlane = ldNormal.Cross (vel.Cross (ldNormal));

	// get direction of drag
	math::Vector3 dragDirection = -velInLDPlane;
	dragDirection.Normalize ();

	// get direction of lift
	math::Vector3 liftDirection = ldNormal.Cross (velInLDPlane);
	liftDirection.Normalize ();
	//std::cerr<<"\n liftDirection: "<<liftDirection;

	// get direction of moment
	math::Vector3 momentDirection = ldNormal;

	double cosAlpha = math::clamp (forwardI.Dot (velInLDPlane) / (forwardI.GetLength () * velInLDPlane.GetLength ()),
	                               -1.0, 1.0);

	// get sign of alpha
	// take upwards component of velocity in lift-drag plane.
	// if sign == upward, then alpha is negative
	double alphaSign = -upwardI.Dot (velInLDPlane) / (upwardI.GetLength () + velInLDPlane.GetLength ());

	// double sinAlpha = sqrt(1.0 - cosAlpha * cosAlpha);
	if (alphaSign > 0.0)
		this->alpha = this->alpha0 + acos (cosAlpha);
	else
		this->alpha = this->alpha0 - acos (cosAlpha);

	// normalize to within +/-90 deg
	while (fabs (this->alpha) > 0.5 * M_PI)
		this->alpha = this->alpha > 0 ? this->alpha - M_PI : this->alpha + M_PI;

	// compute dynamic pressure
	double speedInLDPlane = velInLDPlane.GetLength ();
	double q = 0.5 * this->rho * speedInLDPlane * speedInLDPlane;

	// compute cl at cp, check for stall, correct for sweep
	double cl;
	if (this->alpha > this->alphaStall)
	{
		cl = (this->cla * this->alphaStall + this->claStall * (this->alpha - this->alphaStall)) * cosSweepAngle2;
		// make sure cl is still great than 0
		cl = std::max (0.0, cl);
	}
	else if (this->alpha < -this->alphaStall)
	{
		cl = (-this->cla * this->alphaStall + this->claStall * (this->alpha + this->alphaStall)) * cosSweepAngle2;
		// make sure cl is still less than 0
		cl = std::min (0.0, cl);
	}
	else
		cl = this->cla * this->alpha * cosSweepAngle2;

	// compute lift force at cp
	math::Vector3 lift = cl * q * this->area * liftDirection;

	// compute cd at cp, check for stall, correct for sweep
	double cd;
	if (this->alpha > this->alphaStall)
	{
		cd = (this->cda * this->alphaStall + this->cdaStall * (this->alpha - this->alphaStall)) * cosSweepAngle2;
	}
	else if (this->alpha < -this->alphaStall)
	{
		cd = (-this->cda * this->alphaStall + this->cdaStall * (this->alpha + this->alphaStall)) * cosSweepAngle2;
	}
	else
		cd = (this->cda * this->alpha) * cosSweepAngle2;

	// make sure drag is positive
	cd = fabs (cd);

	// drag at cp
	math::Vector3 drag = cd * q * this->area * dragDirection;

	// compute cm at cp, check for stall, correct for sweep
	double cm;
	if (this->alpha > this->alphaStall)
	{
		cm = (this->cma * this->alphaStall + this->cmaStall * (this->alpha - this->alphaStall)) * cosSweepAngle2;
		// make sure cm is still great than 0
		cm = std::max (0.0, cm);
	}
	else if (this->alpha < -this->alphaStall)
	{
		cm = (-this->cma * this->alphaStall + this->cmaStall * (this->alpha + this->alphaStall)) * cosSweepAngle2;
		// make sure cm is still less than 0
		cm = std::min (0.0, cm);
	}
	else
		cm = this->cma * this->alpha * cosSweepAngle2;

	// reset cm to zero, as cm needs testing
	cm = 0.0;

	// compute moment (torque) at cp
	math::Vector3 moment = cm * q * this->area * momentDirection;

	// moment arm from cg to cp in inertial plane
	math::Vector3 momentArm = pose.rot.RotateVector (this->cp - this->link->GetInertial ()->GetCoG ());
	// gzerr << this->cp << " : " << this->link->GetInertial()->GetCoG() << "\n";

	// force and torque about cg in inertial frame
	math::Vector3 force = lift + drag;

	math::Vector3 torque = moment;

	// apply forces at cg (with torques for position shift)
	this->link->AddForceAtRelativePosition (force, this->cp);
	//this->link->AddTorque(torque);
}

void
USV_Sailing_Plugin::OnUpdateKeel ()
{

	math::Vector3 vel = this->link->GetWorldLinearVel (this->cp) - waterCurrent;

	if (vel.GetLength () <= 0.01)
		return;

	// pose of body
	math::Pose pose = this->link->GetWorldPose ();

	// rotate forward and upward vectors into inertial frame
	math::Vector3 forwardI = pose.rot.RotateVector (this->forward);
	math::Vector3 upwardI = pose.rot.RotateVector (this->upward);

	// ldNormal vector to lift-drag-plane described in inertial frame
	math::Vector3 ldNormal = forwardI.Cross (upwardI).Normalize ();

	// check sweep (angle between vel and lift-drag-plane)
	double sinSweepAngle = ldNormal.Dot (vel) / vel.GetLength ();

	// get cos from trig identity
	double cosSweepAngle2 = (1.0 - sinSweepAngle * sinSweepAngle);
	this->sweep = asin (sinSweepAngle);

	// truncate sweep to within +/-90 deg
	while (fabs (this->sweep) > 0.5 * M_PI)
		this->sweep = this->sweep > 0 ? this->sweep - M_PI : this->sweep + M_PI;

	// angle of attack is the angle between
	// vel projected into lift-drag plane
	//  and
	// forward vector
	//
	// projected = ldNormal Xcross ( vector Xcross ldNormal)
	//
	// so,
	// velocity in lift-drag plane (expressed in inertial frame) is:
	math::Vector3 velInLDPlane = ldNormal.Cross (vel.Cross (ldNormal));

	// get direction of drag
	math::Vector3 dragDirection = -velInLDPlane;
	dragDirection.Normalize ();

	// get direction of lift
	math::Vector3 liftDirection = ldNormal.Cross (velInLDPlane);
	liftDirection.Normalize ();
	//std::cerr<<"\n liftDirection: "<<liftDirection;

	// get direction of moment
	math::Vector3 momentDirection = ldNormal;

	double cosAlpha = math::clamp (forwardI.Dot (velInLDPlane) / (forwardI.GetLength () * velInLDPlane.GetLength ()),
	                               -1.0, 1.0);

	// get sign of alpha
	// take upwards component of velocity in lift-drag plane.
	// if sign == upward, then alpha is negative
	double alphaSign = -upwardI.Dot (velInLDPlane) / (upwardI.GetLength () + velInLDPlane.GetLength ());

	// double sinAlpha = sqrt(1.0 - cosAlpha * cosAlpha);
	if (alphaSign > 0.0)
		this->alpha = this->alpha0 + acos (cosAlpha);
	else
		this->alpha = this->alpha0 - acos (cosAlpha);

	// normalize to within +/-90 deg
	while (fabs (this->alpha) > M_PI)
		this->alpha = this->alpha > 0 ? this->alpha - 2 * M_PI : this->alpha + 2 * M_PI;

	// compute dynamic pressure
	double speedInLDPlane = velInLDPlane.GetLength ();
	double q = 0.5 * this->rho * speedInLDPlane * speedInLDPlane;

	// compute cl at cp, check for stall, correct for sweep
	double cl;
	cl = 8 * sin (2 * this->alpha);
	// compute lift force at cp
	math::Vector3 lift = cl * q * this->area * liftDirection;

	// compute cd at cp, check for stall, correct for sweep
	double cd;
	// make sure drag is positive
	//cd = fabs(cd);

	cd = 2 * (1 - cos (2 * this->alpha));
	// drag at cp
	math::Vector3 drag = cd * q * this->area * dragDirection;

	// compute cm at cp, check for stall, correct for sweep
	double cm;
	if (this->alpha > this->alphaStall)
	{
		cm = (this->cma * this->alphaStall + this->cmaStall * (this->alpha - this->alphaStall)) * cosSweepAngle2;
		// make sure cm is still great than 0
		cm = std::max (0.0, cm);
	}
	else if (this->alpha < -this->alphaStall)
	{
		cm = (-this->cma * this->alphaStall + this->cmaStall * (this->alpha + this->alphaStall)) * cosSweepAngle2;
		// make sure cm is still less than 0
		cm = std::min (0.0, cm);
	}
	else
		cm = this->cma * this->alpha * cosSweepAngle2;

	// reset cm to zero, as cm needs testing
	cm = 0.0;

	// compute moment (torque) at cp
	math::Vector3 moment = cm * q * this->area * momentDirection;

	// moment arm from cg to cp in inertial plane
	math::Vector3 momentArm = pose.rot.RotateVector (this->cp - this->link->GetInertial ()->GetCoG ());

	// force and torque about cg in inertial frame
	math::Vector3 force = lift + drag;
	// + moment.Cross(momentArm);

	math::Vector3 torque = moment;

	// apply forces at cg (with torques for position shift)
	this->link->AddForceAtRelativePosition (force, this->cp);

}

void
USV_Sailing_Plugin::OnUpdateSail ()
{

	this->joint->SetLowStop (0, gazebo::math::Angle (-this->angle));
	this->joint->SetHighStop (0, gazebo::math::Angle (this->angle));
	math::Vector3 aw = wind - this->link->GetWorldLinearVel (this->cp);

	if (aw.GetLength () <= 0.01)
		return;

	// pose of body
	math::Pose pose = this->link->GetWorldPose ();

	// rotate forward and upward vectors into inertial frame
	math::Vector3 forwardI = pose.rot.RotateVector (this->forward); //xb
	math::Vector3 upwardI = pose.rot.RotateVector (this->upward);   //yb

	// ldNormal vector to lift-drag-plane described in inertial frame
	math::Vector3 ldNormal = forwardI.Cross (upwardI).Normalize ();
	// TODOS ESSES PRODUTOS VETORIAIS SÃO PRA PEGAR OS VETORES PERPENDICULARES

	//math::Vector3 velInLDPlane = ldNormal.Cross(aw.Cross(ldNormal)); // isso é igual ao vel, só que escalado????
	math::Vector3 velInLDPlane = aw;
	// get direction of drag
	math::Vector3 dragDirection = velInLDPlane;
	dragDirection.Normalize ();

	// get direction of lift
	// math::Vector3 liftDirection = ldNormal.Cross(velInLDPlane);
	math::Vector3 liftDirection = -ldNormal.Cross (velInLDPlane);
	liftDirection.Normalize ();

	// get direction of moment
	math::Vector3 momentDirection = ldNormal;

	double cosAlpha = math::clamp (forwardI.Dot (velInLDPlane) / (forwardI.GetLength () * velInLDPlane.GetLength ()),
	                               -1.0, 1.0);

	// get sign of alpha
	// take upwards component of velocity in lift-drag plane.
	// if sign == upward, then alpha is negative
	double alphaSign = -upwardI.Dot (velInLDPlane) / (upwardI.GetLength () + velInLDPlane.GetLength ());

	// double sinAlpha = sqrt(1.0 - cosAlpha * cosAlpha);
	if (alphaSign > 0.0)
		this->alpha = acos (cosAlpha);
	else
		this->alpha = -acos (cosAlpha);

	// compute dynamic pressure
	double speedInLDPlane = velInLDPlane.GetLength ();
	double q = 0.5 * this->rho * speedInLDPlane * speedInLDPlane;

	// compute cl at cp, check for stall, correct for sweep

	double cl;

	cl = 1.5 * sin (2 * this->alpha);
	// compute lift force at cp
	math::Vector3 lift = cl * q * this->area * liftDirection;

	// compute cd at cp, check for stall, correct for sweep
	double cd;
	// make sure drag is positive
	//cd = fabs(cd);

	cd = 0.5 * (1 - cos (2 * this->alpha));
	// drag at cp
	math::Vector3 drag = cd * q * this->area * dragDirection;

	// compute cm at cp, check for stall, correct for sweep
	double cm;
	// reset cm to zero, as cm needs testing
	cm = 0.0;

	// compute moment (torque) at cp
	math::Vector3 moment = cm * q * this->area * momentDirection;

	// moment arm from cg to cp in inertial plane
	math::Vector3 momentArm = pose.rot.RotateVector (this->cp - this->link->GetInertial ()->GetCoG ());

	// force and torque about cg in inertial frame
	math::Vector3 force = lift + drag;
	// + moment.Cross(momentArm);

	math::Vector3 torque = moment;

	// apply forces at cg (with torques for position shift)
	this->link->AddForceAtRelativePosition (force, this->cp);
}

void
USV_Sailing_Plugin::ReadWaterCurrent (const geometry_msgs::Vector3::ConstPtr& _msg)
{
	waterCurrent.x = _msg->x;
	waterCurrent.y = _msg->y;
	waterCurrent.z = _msg->z;
}

void
USV_Sailing_Plugin::WaterThreadLoop ()
{
	ros::Rate r (10);
	while (running)
	{
		gazebo::math::Pose pose = this->link->GetWorldCoGPose ();
		usv_water_current::GetSpeed srv;
		srv.request.x = pose.pos.x;
		srv.request.y = pose.pos.y;
		if (velocity_serviceClient_.call (srv))
		{
			waterCurrent.x = srv.response.x;
			waterCurrent.y = srv.response.y;
			//std::cout << "\n ============== fluidWater "<<model_name<<"="<<link->GetName()<<" ("<<fluid_velocity_.x<<", "<<fluid_velocity_.y<<")";
		}
		else
		{
			ROS_WARN ("Failed to call service waterCurrent %s", this->modelName.c_str ());

			ros::Rate s (1);
			s.sleep ();
		}
		r.sleep ();
	}
}

void
USV_Sailing_Plugin::WindThreadLoop ()
{
	ros::Rate r (10);
	while (running)
	{
		gazebo::math::Pose pose = this->link->GetWorldCoGPose ();
		usv_wind_current::GetSpeed srv;
		srv.request.x = pose.pos.x;
		srv.request.y = pose.pos.y;
		if (velocity_serviceClient_.call (srv))
		{
			wind.x = srv.response.x;
			wind.y = srv.response.y;
			//std::cout << "\n ============== fluidWind "<<model_name<<"="<<link->GetName()<<" ("<<fluid_velocity_.x<<", "<<fluid_velocity_.y<<")";
		}
		else
		{
			ROS_WARN ("Failed to call service windCurrent %s", this->modelName.c_str ());

			ros::Rate s (1);
			s.sleep ();
		}
		r.sleep ();
	}
}

