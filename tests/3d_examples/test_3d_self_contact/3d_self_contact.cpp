/**
 * @file 	3d_self_contact.cpp
 * @brief 	This is the test to check self contact for solid dynamics
 * @author 	Xiangyu Hu
 */

#include "sphinxsys.h"
using namespace SPH;
//----------------------------------------------------------------------
//	Set the file path to the data file.
//----------------------------------------------------------------------
std::string full_path_to_file = "./input/coil.stl";
//----------------------------------------------------------------------
//	Basic geometry parameters and numerical setup.
//----------------------------------------------------------------------
Real half_width = 55.0;
Real resolution_ref = half_width / 30.0;
Real BW = resolution_ref * 4;
Vec3d domain_lower_bound(-half_width - BW, -half_width - 1.5 * BW, -BW);
Vec3d domain_upper_bound(half_width + BW, half_width + BW, 2.0 * half_width + BW);
// Domain bounds of the system.
BoundingBox system_domain_bounds(domain_lower_bound, domain_upper_bound);
//----------------------------------------------------------------------
//	Global parameters for material properties.
//----------------------------------------------------------------------
Real rho0_s = 1.265;
Real poisson = 0.45;
Real Youngs_modulus = 5e4;
Real physical_viscosity = 200.0;
//----------------------------------------------------------------------
//	Body shapes used in the case.
//----------------------------------------------------------------------
class Coil : public ComplexShape
{
public:
	explicit Coil(const std::string &shape_name) : ComplexShape(shape_name)
	{
		add<TriangleMeshShapeSTL>(full_path_to_file, Vecd(0), 1.0);
	}
};
class StationaryPlate : public ComplexShape
{
public:
	explicit StationaryPlate(const std::string &shape_name) : ComplexShape(shape_name)
	{
		Vecd halfsize_plate(half_width + BW, 0.5 * BW, half_width + BW);
		Vecd translation_plate(0.0, -half_width - 0.75 * BW, half_width);
		add<TransformShape<GeometricShapeBox>>(translation_plate, halfsize_plate);
	}
};
//----------------------------------------------------------------------
//	The main program
//----------------------------------------------------------------------
int main(int ac, char *av[])
{
	//----------------------------------------------------------------------
	//	Build up -- a SPHSystem
	//----------------------------------------------------------------------
	SPHSystem system(system_domain_bounds, resolution_ref);
	// Tag for run particle relaxation for the initial body fitted distribution.
	system.run_particle_relaxation_ = false;
	// Tag for reload initially repaxed particles.
	system.reload_particles_ = true;
// handle command line arguments
#ifdef BOOST_AVAILABLE
	system.handleCommandlineOptions(ac, av);
#endif
	// output environment
	InOutput in_output(system);
	//----------------------------------------------------------------------
	//	Creating body, materials and particles.
	//----------------------------------------------------------------------
	SolidBody coil(system, makeShared<Coil>("Coil"));
	coil.defineBodyLevelSetShape()->writeLevelSet(coil);
	coil.defineParticlesAndMaterial<ElasticSolidParticles, NeoHookeanSolid>(rho0_s, Youngs_modulus, poisson);
	(!system.run_particle_relaxation_ && system.reload_particles_)
		? coil.generateParticles<ParticleGeneratorReload>(in_output, coil.getBodyName())
		: coil.generateParticles<ParticleGeneratorLattice>();

	SolidBody stationary_plate(system, makeShared<StationaryPlate>("StationaryPlate"));
	stationary_plate.defineParticlesAndMaterial<SolidParticles, LinearElasticSolid>(rho0_s, Youngs_modulus, poisson);
	stationary_plate.generateParticles<ParticleGeneratorLattice>();
	//----------------------------------------------------------------------
	//	Define simple file input and outputs functions.
	//----------------------------------------------------------------------
	BodyStatesRecordingToVtp write_states(in_output, system.real_bodies_);
	//----------------------------------------------------------------------
	//	Define body relation map.
	//	The contact map gives the topological connections between the bodies.
	//	Basically the the range of bodies to build neighbor particle lists.
	//----------------------------------------------------------------------
	BodyRelationInner coil_inner(coil);
	SolidBodyRelationSelfContact coil_self_contact(coil);
	SolidBodyRelationContact coil_contact(coil_self_contact, {&stationary_plate});
	//----------------------------------------------------------------------
	//	check whether run particle relaxation for body fitted particle distribution.
	//----------------------------------------------------------------------
	if (system.run_particle_relaxation_)
	{
		//----------------------------------------------------------------------
		//	Methods used for particle relaxation.
		//----------------------------------------------------------------------
		// Random reset the insert body particle position.
		RandomizeParticlePosition random_inserted_body_particles(coil);
		// Write the particle reload files.
		ReloadParticleIO write_particle_reload_files(in_output, {&coil});
		// A  Physics relaxation step.
		relax_dynamics::RelaxationStepInner relaxation_step_inner(coil_inner);
		//----------------------------------------------------------------------
		//	Particle relaxation starts here.
		//----------------------------------------------------------------------
		random_inserted_body_particles.parallel_exec(0.25);
		relaxation_step_inner.surface_bounding_.parallel_exec();
		write_states.writeToFile(0);
		//----------------------------------------------------------------------
		//	Particle relaxation loop.
		//----------------------------------------------------------------------
		int ite_p = 0;
		while (ite_p < 1000)
		{
			relaxation_step_inner.parallel_exec();
			ite_p += 1;
			if (ite_p % 200 == 0)
			{
				std::cout << std::fixed << std::setprecision(9) << "Relaxation steps for the inserted body N = " << ite_p << "\n";
				write_states.writeToFile(ite_p);
			}
		}
		std::cout << "The physics relaxation process of inserted body finish !" << std::endl;
		// Output particles position for reload.
		write_particle_reload_files.writeToFile(0);
		return 0;
	}
	//----------------------------------------------------------------------
	//	This section define all numerical methods will be used in this case.
	//----------------------------------------------------------------------
	Gravity gravity(Vecd(0.0, -1.0, 0.0));
	// initialize a time step
	TimeStepInitialization initialization_with_gravity(coil, gravity);
	// Corrected configuration for reproducing rigid rotation.
	solid_dynamics::CorrectConfiguration corrected_configuration(coil_inner);
	// Time step size
	solid_dynamics::AcousticTimeStepSize computing_time_step_size(coil);
	// stress relaxation.
	solid_dynamics::StressRelaxationFirstHalf stress_relaxation_first_half(coil_inner);
	solid_dynamics::StressRelaxationSecondHalf stress_relaxation_second_half(coil_inner);
	// Algorithms for solid-solid contacts.
	solid_dynamics::ContactDensitySummation coil_update_contact_density(coil_contact);
	solid_dynamics::ContactForceFromWall coil_compute_solid_contact_forces(coil_contact);
	solid_dynamics::SelfContactDensitySummation coil_self_contact_density(coil_self_contact);
	solid_dynamics::SelfContactForce coil_self_contact_forces(coil_self_contact);
	// Damping the velocity field for quasi-static solution
	DampingWithRandomChoice<DampingPairwiseInner<Vec3d>>
		coil_damping(0.2, coil_inner, "Velocity", physical_viscosity);
	//----------------------------------------------------------------------
	//	From here the time stepping begins.
	//----------------------------------------------------------------------
	system.initializeSystemCellLinkedLists();
	system.initializeSystemConfigurations();
	// apply initial condition
	corrected_configuration.parallel_exec();
	write_states.writeToFile(0);
	// Setup time stepping control parameters.
	int ite = 0;
	Real end_time = 10.0;
	Real output_period = end_time / 100.0;
	Real dt = 0.0;
	// Statistics for computing time.
	tick_count t1 = tick_count::now();
	tick_count::interval_t interval;
	//----------------------------------------------------------------------
	//	Main loop
	//----------------------------------------------------------------------
	while (GlobalStaticVariables::physical_time_ < end_time)
	{
		Real integration_time = 0.0;
		while (integration_time < output_period)
		{
			if (ite % 100 == 0)
			{
				std::cout << "N=" << ite << " Time: "
						  << GlobalStaticVariables::physical_time_ << "	dt: "
						  << dt << "\n";
			}
			initialization_with_gravity.parallel_exec();
			// contact dynamics.
			coil_self_contact_density.parallel_exec();
			coil_self_contact_forces.parallel_exec();
			coil_update_contact_density.parallel_exec();
			coil_compute_solid_contact_forces.parallel_exec();
			// Stress relaxation and damping.
			stress_relaxation_first_half.parallel_exec(dt);
			coil_damping.parallel_exec(dt);
			stress_relaxation_second_half.parallel_exec(dt);

			ite++;
			dt = computing_time_step_size.parallel_exec();
			integration_time += dt;
			GlobalStaticVariables::physical_time_ += dt;

			// update particle neighbor relations for contact dynamics
			coil.updateCellLinkedList();
			coil_self_contact.updateConfiguration();
			coil_contact.updateConfiguration();
		}
		tick_count t2 = tick_count::now();
		write_states.writeToFile();
		tick_count t3 = tick_count::now();
		interval += t3 - t2;
	}
	tick_count t4 = tick_count::now();

	tick_count::interval_t tt;
	tt = t4 - t1 - interval;
	std::cout << "Total wall time for computation: " << tt.seconds() << " seconds." << std::endl;

	return 0;
}
