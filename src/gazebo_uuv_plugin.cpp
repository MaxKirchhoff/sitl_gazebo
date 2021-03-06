/*
 * Copyright 2016 Austin Buchan, TUHH Hamburg, Germany
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gazebo_uuv_plugin.h"

namespace gazebo {

GazeboUUVPlugin::~GazeboUUVPlugin() {
  event::Events::DisconnectWorldUpdateBegin(update_connection_);
}

void GazeboUUVPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf) {
  namespace_.clear();
  getSdfParam<std::string>(
    _sdf, "robotNamespace", namespace_, namespace_, true);
  
  node_handle_ = transport::NodePtr(new transport::Node());
  node_handle_->Init(namespace_);

  getSdfParam<std::string>(_sdf, "linkName", link_name_, link_name_, true);
  link_ = _model->GetLink(link_name_);
  rotor_links_ = link_->GetChildJointsLinks();
  for(int i = 0; i < rotor_links_.size(); i++) {
    std::cout << "Rotor Link:" << rotor_links_[i]->GetScopedName() << "\n";
    command_[i] = 0.0;
  }

  getSdfParam<std::string>(
    _sdf, "commandSubTopic", command_sub_topic_, command_sub_topic_);
  
  command_sub_ = node_handle_->Subscribe<mav_msgs::msgs::CommandMotorSpeed>(
    "~/" + _model->GetName() + command_sub_topic_, &GazeboUUVPlugin::CommandCallback, this);

  update_connection_ = event::Events::ConnectWorldUpdateBegin(
    boost::bind(&GazeboUUVPlugin::OnUpdate, this, _1));

  getSdfParam<double>(
    _sdf, "motorForceConstant", motor_force_constant_, motor_force_constant_);

  getSdfParam<double>(
    _sdf, "motorTorqueConstant", motor_torque_constant_, motor_torque_constant_);

  math::Vector3 added_mass_linear(0,0,0);
  getSdfParam<math::Vector3>(
    _sdf, "addedMassLinear", added_mass_linear, added_mass_linear);
  X_udot_ = added_mass_linear[0];
  Y_vdot_ = added_mass_linear[1];
  Z_wdot_ = added_mass_linear[2];

  math::Vector3 added_mass_angular(0,0,0);
  getSdfParam<math::Vector3>(
    _sdf, "addedMassAngular", added_mass_angular, added_mass_angular);
  K_pdot_ = added_mass_angular[0];
  M_qdot_ = added_mass_angular[1];
  N_rdot_ = added_mass_angular[2];

  math::Vector3 damping_linear(0,0,0);
  getSdfParam<math::Vector3>(
    _sdf, "dampingLinear", damping_linear, damping_linear);
  X_u_ = damping_linear[0];
  Y_v_ = damping_linear[1];
  Z_w_ = damping_linear[2];
  
  math::Vector3 damping_angular(0,0,0);
  getSdfParam<math::Vector3>(
    _sdf, "dampingAngular", damping_angular, damping_angular);
  K_p_ = damping_angular[0];
  M_q_ = damping_angular[1];
  N_r_ = damping_angular[2];

}

void GazeboUUVPlugin::CommandCallback(CommandMotorSpeedPtr &command) {
  for (int i = 0; i < 4; i++) {
    command_[i] = command->motor_speed(i);
  }
  /*std::cout << "UUV Command Callback:" 
    << command_[0] << ","
    << command_[1] << ","
    << command_[2] << ","
    << command_[3] << ","
    << "\n";*/
}

void GazeboUUVPlugin::OnUpdate(const common::UpdateInfo& _info) {
  double now = _info.simTime.Double();
  time_delta_ =  now - last_time_;
  last_time_ = now;

  //std::cout << "UUV Update at " << now << ", delta " << time_delta_ << "\n";
  
  double forces[4];
  double torques[4];

  // Apply forces and torques at rotor joints
  for(int i = 0; i < 4; i++) {
    
    // Currently a rotor index hack to get over IMU link being first
    math::Vector3 rotor_force(0, 0, motor_force_constant_ * command_[i]);
    rotor_links_[i+1]->AddRelativeForce(rotor_force);
    
    forces[i] = rotor_force[2];
    //std::cout << "Applying force " << rotor_force[2] << " to rotor " << i << "\n";

    // Assume even-indexed props are CW, odd-indexed CCW. Apply drag torque
    // directly to main body X axis
    int propeller_direction = (i%2==0)?1:-1;
    math::Vector3 rotor_torque(
      propeller_direction * motor_torque_constant_ * command_[i], 0, 0);
    link_->AddRelativeTorque(rotor_torque);

    //std::cout << "Applying torque " << rotor_torque[2] << " to rotor " << i << "\n";
    torques[i] = rotor_torque[0];
  }
  
  /*
  std::cout << "Forces:";
  for(int i = 0; i<4; i++) std::cout << forces[i] << ",";
  std::cout << "\n";
  */

  /*
  std::cout << "Torques:";
  for(int i = 0; i<4; i++) std::cout << torques[i] << ",";
  std::cout << "\n";
  */

  // Calculate and apply body Coriolis and Drag forces and torques
  math::Vector3 linear_velocity = link_->GetRelativeLinearVel();
  double u = linear_velocity[0];
  double v = linear_velocity[1];
  double w = linear_velocity[2];

  math::Vector3 angular_velocity = link_->GetRelativeAngularVel();
  double p = angular_velocity[0];
  double q = angular_velocity[1];
  double r = angular_velocity[2];

  //std::cout << "Vels:" << linear_velocity << ":" << angular_velocity << "\n"; 

  // Forces from linear velocity
  math::Matrix3 C_AD_FL(
    -X_u_, 0, 0,
    0, -Y_v_, 0, 
    0, 0, -Z_w_
  );

  // Forces from angular velocity (identical to torque from linear velocity).
  math::Matrix3 C_AD_FA(
    0, -Z_wdot_ * w, Y_vdot_ * v,
    Z_wdot_ * w, 0, -X_udot_ * u,
    -Y_vdot_ * v, X_udot_ * u, 0
  );

  // Torques from angular velocity
  math::Matrix3 C_AD_TA(
    -K_p_, -N_rdot_ * r,  M_qdot_ * q,
    N_rdot_ * r, -M_q_, -K_pdot_ * p,
    -M_qdot_ * q, K_pdot_ * p, -N_r_
  );
 
  math::Vector3 coriolis_force =
    (C_AD_FL*linear_velocity) + (C_AD_FA*angular_velocity);
  math::Vector3 coriolis_torque =
    (C_AD_FA*linear_velocity) + (C_AD_TA*angular_velocity);
  
  //std::cout << C_AD_FA << "\n";
  //std::cout << "Linear:" << coriolis_force << "\n";
  //std::cout << "Angular:" << angular_velocity << "\n";
  
  link_->AddRelativeForce(coriolis_force);
  link_->AddRelativeTorque(coriolis_torque);
}

GZ_REGISTER_MODEL_PLUGIN(GazeboUUVPlugin)
}
