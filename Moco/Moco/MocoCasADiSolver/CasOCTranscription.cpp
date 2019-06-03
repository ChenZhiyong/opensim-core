/* -------------------------------------------------------------------------- *
 * OpenSim Moco: CasOCTranscription.cpp                                       *
 * -------------------------------------------------------------------------- *
 * Copyright (c) 2018 Stanford University and the Authors                     *
 *                                                                            *
 * Author(s): Christopher Dembia                                              *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may    *
 * not use this file except in compliance with the License. You may obtain a  *
 * copy of the License at http://www.apache.org/licenses/LICENSE-2.0          *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,          *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 * -------------------------------------------------------------------------- */
#include "CasOCTranscription.h"

using casadi::DM;
using casadi::MX;
using casadi::MXVector;
using casadi::Slice;

namespace CasOC {

void Transcription::createVariablesAndSetBounds(
        const casadi::DM& grid, int numDefectsPerGridPoint) {
    // Set the grid.
    // -------------
    // The grid for a transcription scheme includes both mesh points (i.e.
    // points that lie on the endpoints of a mesh interval) and any
    // additional collocation points that may lie on mesh interior (as in
    // Hermite-Simpson collocation, etc.).
    m_numMeshPoints = (int)m_solver.getMesh().size();
    m_numGridPoints = (int)grid.numel();
    m_numMeshIntervals = m_numMeshPoints - 1;
    m_numPointsIgnoringConstraints = m_numGridPoints - m_numMeshPoints;
    m_numDefectsPerGridPoint = numDefectsPerGridPoint;
    // TODO: Update when supporting prescribed kinematics.
    m_numResiduals =
            m_solver.isDynamicsModeImplicit() ? m_problem.getNumSpeeds() : 0;
    m_numConstraints =
            m_numDefectsPerGridPoint * m_numMeshIntervals +
            m_numResiduals * m_numGridPoints +
            m_problem.getNumKinematicConstraintEquations() * m_numMeshPoints;
    m_constraints.path.resize(m_problem.getPathConstraintInfos().size());
    for (int ipc = 0; ipc < (int)m_constraints.path.size(); ++ipc) {
        const auto& info = m_problem.getPathConstraintInfos()[ipc];
        m_numConstraints += info.size() * m_numMeshPoints;
    }
    m_grid = grid;

    // Create variables.
    // -----------------
    m_vars[initial_time] = MX::sym("initial_time");
    m_vars[final_time] = MX::sym("final_time");
    m_duration = m_vars[final_time] - m_vars[initial_time];
    m_times = createTimes(m_vars[initial_time], m_vars[final_time]);
    m_vars[states] =
            MX::sym("states", m_problem.getNumStates(), m_numGridPoints);
    m_vars[controls] =
            MX::sym("controls", m_problem.getNumControls(), m_numGridPoints);
    m_vars[multipliers] = MX::sym(
            "multipliers", m_problem.getNumMultipliers(), m_numGridPoints);
    m_vars[derivatives] = MX::sym(
            "derivatives", m_problem.getNumDerivatives(), m_numGridPoints);
    // TODO: This assumes that slack variables are applied at all
    // collocation points on the mesh interval interior.
    m_vars[slacks] = MX::sym(
            "slacks", m_problem.getNumSlacks(), m_numPointsIgnoringConstraints);
    m_vars[parameters] = MX::sym("parameters", m_problem.getNumParameters(), 1);

    m_paramsTrajGrid = MX::repmat(m_vars[parameters], 1, m_numGridPoints);
    m_paramsTraj = MX::repmat(m_vars[parameters], 1, m_numMeshPoints);
    m_paramsTrajIgnoringConstraints =
            MX::repmat(m_vars[parameters], 1, m_numPointsIgnoringConstraints);

    m_kinematicConstraintIndices = createKinematicConstraintIndices();
    std::vector<int> daeIndicesVector;
    std::vector<int> daeIndicesIgnoringConstraintsVector;
    for (int i = 0; i < m_kinematicConstraintIndices.size2(); ++i) {
        if (m_kinematicConstraintIndices(i).scalar() == 1) {
            daeIndicesVector.push_back(i);
        } else {
            daeIndicesIgnoringConstraintsVector.push_back(i);
        }
    }

    auto makeTimeIndices = [](const std::vector<int>& in) {
        casadi::Matrix<casadi_int> out(1, in.size());
        for (int i = 0; i < (int)in.size(); ++i) { out(i) = in[i]; }
        return out;
    };
    {
        std::vector<int> gridIndicesVector(m_numGridPoints);
        std::iota(gridIndicesVector.begin(), gridIndicesVector.end(), 0);
        m_gridIndices = makeTimeIndices(gridIndicesVector);
    }
    m_daeIndices = makeTimeIndices(daeIndicesVector);
    m_daeIndicesIgnoringConstraints =
            makeTimeIndices(daeIndicesIgnoringConstraintsVector);

    // Set variable bounds.
    // --------------------
    auto initializeBounds = [&](VariablesDM& bounds) {
        for (auto& kv : m_vars) {
            bounds[kv.first] = DM(kv.second.rows(), kv.second.columns());
        }
    };
    initializeBounds(m_lowerBounds);
    initializeBounds(m_upperBounds);

    setVariableBounds(initial_time, 0, 0, m_problem.getTimeInitialBounds());
    setVariableBounds(final_time, 0, 0, m_problem.getTimeFinalBounds());

    {
        const auto& stateInfos = m_problem.getStateInfos();
        int is = 0;
        for (const auto& info : stateInfos) {
            setVariableBounds(
                    states, is, Slice(1, m_numGridPoints - 1), info.bounds);
            // The "0" grabs the first column (first mesh point).
            setVariableBounds(states, is, 0, info.initialBounds);
            // The "-1" grabs the last column (last mesh point).
            setVariableBounds(states, is, -1, info.finalBounds);
            ++is;
        }
    }
    {
        const auto& controlInfos = m_problem.getControlInfos();
        int ic = 0;
        for (const auto& info : controlInfos) {
            setVariableBounds(
                    controls, ic, Slice(1, m_numGridPoints - 1), info.bounds);
            setVariableBounds(controls, ic, 0, info.initialBounds);
            setVariableBounds(controls, ic, -1, info.finalBounds);
            ++ic;
        }
    }
    {
        const auto& multiplierInfos = m_problem.getMultiplierInfos();
        int im = 0;
        for (const auto& info : multiplierInfos) {
            setVariableBounds(multipliers, im, Slice(1, m_numGridPoints - 1),
                    info.bounds);
            setVariableBounds(multipliers, im, 0, info.initialBounds);
            setVariableBounds(multipliers, im, -1, info.finalBounds);
            ++im;
        }
    }
    {
        if (m_solver.isDynamicsModeImplicit()) {
            // "Slice()" grabs everything in that dimension (like ":" in
            // Matlab).
            // TODO: How to choose bounds on udot?
            setVariableBounds(derivatives, Slice(), Slice(), {-1000, 1000});
        }
    }
    {
        const auto& slackInfos = m_problem.getSlackInfos();
        int isl = 0;
        for (const auto& info : slackInfos) {
            setVariableBounds(slacks, isl, Slice(), info.bounds);
            ++isl;
        }
    }
    {
        const auto& paramInfos = m_problem.getParameterInfos();
        int ip = 0;
        for (const auto& info : paramInfos) {
            setVariableBounds(parameters, ip, 0, info.bounds);
            ++ip;
        }
    }
}

void Transcription::transcribe() {

    // Cost.
    // =====
    setObjective();

    // Compute DAEs at necessary grid points.
    // ======================================
    const int NQ = m_problem.getNumCoordinates();
    const int NU = m_problem.getNumSpeeds();
    const int NS = m_problem.getNumStates();
    OPENSIM_THROW_IF(NQ != NU, OpenSim::Exception,
            "Problems with differing numbers of coordinates and speeds are "
            "not supported (e.g., quaternions).");

    // TODO: Does creating all this memory have efficiency implications in
    // CasADi?
    // Initialize memory for state derivatives and defects.
    // ----------------------------------------------------
    m_xdot = MX(NS, m_numGridPoints);
    m_constraints.defects = MX(casadi::Sparsity::dense(
            m_numDefectsPerGridPoint, m_numMeshIntervals));
    m_constraintsLowerBounds.defects =
            DM::zeros(m_numDefectsPerGridPoint, m_numMeshIntervals);
    m_constraintsUpperBounds.defects =
            DM::zeros(m_numDefectsPerGridPoint, m_numMeshIntervals);

    // Initialize memory for implicit residuals.
    // -----------------------------------------
    if (m_solver.isDynamicsModeImplicit()) {
        const auto& NR = m_numResiduals;
        m_constraints.residuals =
                MX(casadi::Sparsity::dense(NR, m_numGridPoints));
        m_constraintsLowerBounds.residuals = DM::zeros(NR, m_numGridPoints);
        m_constraintsUpperBounds.residuals = DM::zeros(NR, m_numGridPoints);
    }

    // Initialize memory for kinematic constraints.
    // --------------------------------------------
    int numKinematicConstraints =
            m_problem.getNumKinematicConstraintEquations();
    m_constraints.kinematic = MX(
            casadi::Sparsity::dense(numKinematicConstraints, m_numMeshPoints));

    const auto& kcBounds = m_problem.getKinematicConstraintBounds();
    m_constraintsLowerBounds.kinematic = casadi::DM::repmat(
            kcBounds.lower, numKinematicConstraints, m_numMeshPoints);
    m_constraintsUpperBounds.kinematic = casadi::DM::repmat(
            kcBounds.upper, numKinematicConstraints, m_numMeshPoints);

    // qdot
    // ----
    const MX u = m_vars[states](Slice(NQ, NQ + NU), Slice());
    m_xdot(Slice(0, NQ), Slice()) = u;

    if (m_problem.getEnforceConstraintDerivatives() &&
            m_numPointsIgnoringConstraints) {
        // In Hermite-Simpson, we must compute a velocity correction at all mesh
        // interval midpoints and update qdot. See MocoCasADiVelocityCorrection
        // for more details. This function only takes multibody state variables:
        // coordinates and speeds.
        // TODO: The points at which we apply the velocity correction
        // are correct for Trapezoidal (no points) and Hermite-Simpson (mesh
        // interval midpoints), but might not be correct in general. Revisit
        // this if we add other transcription schemes.
        const auto velocityCorrOut = evalOnTrajectory(
                m_problem.getVelocityCorrection(), {multibody_states, slacks},
                m_daeIndicesIgnoringConstraints);
        const auto uCorr = velocityCorrOut.at(0);

        m_xdot(Slice(0, NQ), m_daeIndicesIgnoringConstraints) += uCorr;
    }

    // udot, zdot, residual, kcerr
    // ---------------------------
    if (m_solver.isDynamicsModeImplicit()) {

        // udot.
        const MX w = m_vars[derivatives];
        m_xdot(Slice(NQ, NQ + NU), Slice()) = w;

        std::vector<Var> inputs{states, controls, multipliers, derivatives};

        // When the model has kinematic constraints, we must treat grid points
        // differently, as kinematic constraints are computed for only some
        // grid points. When the model does *not* have kinematic constraints,
        // the DAE is the same for all grid points, but the evaluation is still
        // done separately to keep implementation general.

        // residual, zdot, kcerr
        // Points where we compute algebraic constraints.
        {
            const auto out =
                    evalOnTrajectory(m_problem.getImplicitMultibodySystem(),
                            inputs, m_daeIndices);
            m_constraints.residuals(Slice(), m_daeIndices) = out.at(0);
            // zdot.
            m_xdot(Slice(NQ + NU, NS), m_daeIndices) = out.at(1);
            m_constraints.kinematic = out.at(2);
        }

        // Points where we ignore algebraic constraints.
        if (m_numPointsIgnoringConstraints) {
            const auto out = evalOnTrajectory(
                    m_problem.getImplicitMultibodySystemIgnoringConstraints(),
                    inputs, m_daeIndicesIgnoringConstraints);
            m_constraints.residuals(Slice(), m_daeIndicesIgnoringConstraints) =
                    out.at(0);
            // zdot.
            m_xdot(Slice(NQ + NU, NS), m_daeIndicesIgnoringConstraints) =
                    out.at(1);
        }

    } else { // Explicit dynamics mode.
        std::vector<Var> inputs{states, controls, multipliers, derivatives};

        // udot, zdot, kcerr.
        // Points where we compute algebraic constraints.
        {
            // Evaluate the multibody system function and get udot
            // (speed derivatives) and zdot (auxiliary derivatives).
            const auto out = evalOnTrajectory(
                    m_problem.getMultibodySystem(), inputs, m_daeIndices);
            m_xdot(Slice(NQ, NQ + NU), m_daeIndices) = out.at(0);
            m_xdot(Slice(NQ + NU, NS), m_daeIndices) = out.at(1);
            m_constraints.kinematic = out.at(2);
        }

        // Points where we ignore algebraic constraints.
        if (m_numPointsIgnoringConstraints) {
            const auto out = evalOnTrajectory(
                    m_problem.getMultibodySystemIgnoringConstraints(), inputs,
                    m_daeIndicesIgnoringConstraints);
            m_xdot(Slice(NQ, NQ + NU), m_daeIndicesIgnoringConstraints) =
                    out.at(0);
            m_xdot(Slice(NQ + NU, NS), m_daeIndicesIgnoringConstraints) =
                    out.at(1);
        }
    }

    // Calculate defects.
    // ------------------
    calcDefects();

    // Path constraints
    // ----------------
    // The individual path constraint functions are passed to CasADi to
    // maximize CasADi's ability to take derivatives efficiently.
    int numPathConstraints = (int)m_problem.getPathConstraintInfos().size();
    m_constraints.path.resize(numPathConstraints);
    m_constraintsLowerBounds.path.resize(numPathConstraints);
    m_constraintsUpperBounds.path.resize(numPathConstraints);
    for (int ipc = 0; ipc < (int)m_constraints.path.size(); ++ipc) {
        const auto& info = m_problem.getPathConstraintInfos()[ipc];
        // TODO: Is it sufficiently general to apply these to mesh points?
        const auto out = evalOnTrajectory(*info.function,
                {states, controls, multipliers, derivatives}, m_daeIndices);
        m_constraints.path[ipc] = out.at(0);
        m_constraintsLowerBounds.path[ipc] =
                casadi::DM::repmat(info.lowerBounds, 1, m_numMeshPoints);
        m_constraintsUpperBounds.path[ipc] =
                casadi::DM::repmat(info.upperBounds, 1, m_numMeshPoints);
    }
}

void Transcription::setObjective() {
    DM quadCoeffs = this->createQuadratureCoefficients();
    MX integrandTraj;
    {
        // Here, we include evaluations of the integral cost
        // integrand into the symbolic expression graph for the integral
        // cost. We are *not* numerically evaluating the integral cost
        // integrand here--that occurs when the function by casadi::nlpsol()
        // is evaluated.
        integrandTraj = evalOnTrajectory(m_problem.getIntegralCostIntegrand(),
                {states, controls, multipliers, derivatives}, m_gridIndices)
                                .at(0);
    }

    // Minimize Lagrange multipliers if specified by the solver.
    if (m_solver.getMinimizeLagrangeMultipliers() &&
            m_problem.getNumMultipliers()) {
        const auto mults = m_vars[multipliers];
        const double multiplierWeight = m_solver.getLagrangeMultiplierWeight();
        // Sum across constraints of each multiplier element squared.
        integrandTraj += multiplierWeight * MX::sum1(MX::sq(mults));
    }
    MX integralCost = m_duration * dot(quadCoeffs.T(), integrandTraj);

    MXVector endpointCostOut;
    m_problem.getEndpointCost().call(
            {m_vars[final_time], m_vars[states](Slice(), -1),
                    m_vars[controls](Slice(), -1),
                    m_vars[multipliers](Slice(), -1),
                    m_vars[derivatives](Slice(), -1), m_vars[parameters]},
            endpointCostOut);
    const auto endpointCost = endpointCostOut.at(0);

    m_objective = integralCost + endpointCost;
}

Solution Transcription::solve(const Iterate& guessOrig) {

    // Define the NLP.
    // ---------------
    transcribe();

    // Resample the guess.
    // -------------------
    const auto guessTimes = createTimes(guessOrig.variables.at(initial_time),
            guessOrig.variables.at(final_time));
    auto guess = guessOrig.resample(guessTimes);

    // Adjust guesses for the slack variables to ensure they are the correct
    // length (i.e. slacks.size2() == m_numPointsIgnoringConstraints).
    if (guess.variables.find(Var::slacks) != guess.variables.end()) {
        auto& slacks = guess.variables.at(Var::slacks);

        // If slack variables provided in the guess are equal to the grid
        // length, remove the elements on the mesh points where the slack
        // variables are not defined.
        if (slacks.size2() == m_numGridPoints) {
            casadi::DM kinConIndices = createKinematicConstraintIndices();
            std::vector<casadi_int> slackColumnsToRemove;
            for (int itime = 0; itime < m_numGridPoints; ++itime) {
                if (kinConIndices(itime).__nonzero__()) {
                    slackColumnsToRemove.push_back(itime);
                }
            }
            // The first argument is an empty vector since we don't want to
            // remove an entire row.
            slacks.remove(std::vector<casadi_int>(), slackColumnsToRemove);
        }

        // Check that either that the slack variables provided in the guess
        // are the correct length, or that the correct number of columns
        // were removed.
        OPENSIM_THROW_IF(slacks.size2() != m_numPointsIgnoringConstraints,
                OpenSim::Exception,
                OpenSim::format("Expected slack variables to be length %i, "
                                "but they are length %i.",
                        m_numPointsIgnoringConstraints, slacks.size2()));
    }

    // Create the CasADi NLP function.
    // -------------------------------
    // Option handling is copied from casadi::OptiNode::solver().
    casadi::Dict options = m_solver.getPluginOptions();
    if (!options.empty()) {
        options[m_solver.getOptimSolver()] = m_solver.getSolverOptions();
    }
    // The inputs to nlpsol() are symbolic (casadi::MX).
    casadi::MXDict nlp;
    auto x = flattenVariables(m_vars);
    nlp.emplace(std::make_pair("x", x));
    // The m_objective symbolic variable holds an expression graph including
    // all the calculations performed on the variables x.
    nlp.emplace(std::make_pair("f", m_objective));
    // The m_constraints symbolic vector holds all of the expressions for
    // the constraint functions.
    // veccat() concatenates std::vectors into a single MX vector.

    auto g = flattenConstraints(m_constraints);
    nlp.emplace(std::make_pair("g", g));
    if (!m_solver.getWriteSparsity().empty()) {
        const auto prefix = m_solver.getWriteSparsity();
        auto gradient = casadi::MX::gradient(nlp["f"], nlp["x"]);
        gradient.sparsity().to_file(
                prefix + "_objective_gradient_sparsity.mtx");
        auto hessian = casadi::MX::hessian(nlp["f"], nlp["x"]);
        hessian.sparsity().to_file(prefix + "_objective_Hessian_sparsity.mtx");
        auto lagrangian = m_objective +
                          casadi::MX::dot(casadi::MX::ones(nlp["g"].sparsity()),
                                  nlp["g"]);
        auto hessian_lagr = casadi::MX::hessian(lagrangian, nlp["x"]);
        hessian_lagr.sparsity().to_file(
                prefix + "_Lagrangian_Hessian_sparsity.mtx");
        auto jacobian = casadi::MX::jacobian(nlp["g"], nlp["x"]);
        jacobian.sparsity().to_file(
                prefix + "constraint_Jacobian_sparsity.mtx");
    }
    const casadi::Function nlpFunc =
            casadi::nlpsol("nlp", m_solver.getOptimSolver(), nlp, options);

    // Run the optimization (evaluate the CasADi NLP function).
    // --------------------------------------------------------
    // The inputs and outputs of nlpFunc are numeric (casadi::DM).
    const casadi::DMDict nlpResult =
            nlpFunc(casadi::DMDict{{"x0", flattenVariables(guess.variables)},
                    {"lbx", flattenVariables(m_lowerBounds)},
                    {"ubx", flattenVariables(m_upperBounds)},
                    {"lbg", flattenConstraints(m_constraintsLowerBounds)},
                    {"ubg", flattenConstraints(m_constraintsUpperBounds)}});

    // Create a CasOC::Solution.
    // -------------------------
    Solution solution = m_problem.createIterate<Solution>();
    const auto finalVariables = nlpResult.at("x");
    solution.variables = expandVariables(finalVariables);
    solution.objective = nlpResult.at("f").scalar();
    solution.times = createTimes(
            solution.variables[initial_time], solution.variables[final_time]);
    solution.stats = nlpFunc.stats();
    if (!solution.stats.at("success")) {
        // For some reason, nlpResult.at("g") is all 0. So we calculate the
        // constraints ourselves.
        casadi::Function constraintFunc("constraints", {x}, {g});
        casadi::DMVector out;
        constraintFunc.call(casadi::DMVector{finalVariables}, out);
        printConstraintValues(solution, expandConstraints(out[0]));
    }
    return solution;
}

void Transcription::printConstraintValues(
        const Iterate& it, const Constraints<casadi::DM>& constraints) const {

    auto& stream = std::cout;

    // We want to be able to restore the stream's original formatting.
    OpenSim::StreamFormat streamFormat(stream);

    // Find the longest state, control, multiplier, derivative, or slack name.
    auto compareSize = [](const std::string& a, const std::string& b) {
        return a.size() < b.size();
    };
    int maxNameLength = 0;
    auto updateMaxNameLength = [&maxNameLength, compareSize](
                                       const std::vector<std::string>& names) {
        if (!names.empty()) {
            maxNameLength = (int)std::max_element(
                    names.begin(), names.end(), compareSize)
                                    ->size();
        }
    };
    updateMaxNameLength(it.state_names);
    updateMaxNameLength(it.control_names);
    updateMaxNameLength(it.multiplier_names);
    updateMaxNameLength(it.derivative_names);
    updateMaxNameLength(it.slack_names);

    stream << "\nActive or violated continuous variable bounds" << std::endl;
    stream << "L and U indicate which bound is active; "
              "'*' indicates a bound is violated. "
           << std::endl;
    stream << "The case of lower==upper==value is ignored." << std::endl;

    // Bounds on time-varying variables.
    // ---------------------------------
    auto print_bounds = [&maxNameLength](const std::string& description,
                                const std::vector<std::string>& names,
                                const casadi::DM& times,
                                const casadi::DM& values,
                                const casadi::DM& lower,
                                const casadi::DM& upper) {
        stream << "\n" << description << ": ";

        bool boundsActive = false;
        bool boundsViolated = false;
        for (casadi_int ivar = 0; ivar < values.rows(); ++ivar) {
            for (casadi_int itime = 0; itime < times.numel(); ++itime) {
                const auto& L = lower(ivar, itime).scalar();
                const auto& V = values(ivar, itime).scalar();
                const auto& U = upper(ivar, itime).scalar();
                if (V <= L || V >= U) {
                    if (V == L && L == U) continue;
                    boundsActive = true;
                    if (V < L || V > U) {
                        boundsViolated = true;
                        break;
                    }
                }
            }
        }

        if (!boundsActive && !boundsViolated) {
            stream << "no bounds active or violated" << std::endl;
            return;
        }

        if (!boundsViolated) {
            stream << "some bounds active but no bounds violated";
        } else {
            stream << "some bounds active or violated";
        }

        stream << "\n"
               << std::setw(maxNameLength) << "  " << std::setw(9) << "time "
               << "  " << std::setw(9) << "lower"
               << "    " << std::setw(9) << "value"
               << "    " << std::setw(9) << "upper"
               << " " << std::endl;

        for (casadi_int ivar = 0; ivar < values.rows(); ++ivar) {
            for (casadi_int itime = 0; itime < times.numel(); ++itime) {
                const auto& L = lower(ivar, itime).scalar();
                const auto& V = values(ivar, itime).scalar();
                const auto& U = upper(ivar, itime).scalar();
                if (V <= L || V >= U) {
                    // In the case where lower==upper==value, there is no
                    // issue; ignore.
                    if (V == L && L == U) continue;
                    const auto& time = times(itime);
                    stream << std::setw(maxNameLength) << names[ivar] << "  "
                           << std::setprecision(2) << std::scientific
                           << std::setw(9) << time << "  " << std::setw(9) << L
                           << " <= " << std::setw(9) << V
                           << " <= " << std::setw(9) << U << " ";
                    // Show if the constraint is violated.
                    if (V <= L)
                        stream << "L";
                    else
                        stream << " ";
                    if (V >= U)
                        stream << "U";
                    else
                        stream << " ";
                    if (V < L || V > U) stream << "*";
                    stream << std::endl;
                }
            }
        }
    };
    const auto& vars = it.variables;
    const auto& lower = m_lowerBounds;
    const auto& upper = m_upperBounds;
    print_bounds("State bounds", it.state_names, it.times, vars.at(states),
            lower.at(states), upper.at(states));
    print_bounds("Control bounds", it.state_names, it.times, vars.at(controls),
            lower.at(controls), upper.at(controls));
    print_bounds("Multiplier bounds", it.state_names, it.times,
            vars.at(multipliers), lower.at(multipliers), upper.at(multipliers));
    print_bounds("Derivative bounds", it.state_names, it.times,
            vars.at(derivatives), lower.at(derivatives), upper.at(derivatives));
    print_bounds("Slack bounds", it.state_names, it.times, vars.at(slacks),
            lower.at(slacks), upper.at(slacks));

    // Bounds on time and parameter variables.
    // ---------------------------------------
    maxNameLength = 0;
    updateMaxNameLength(it.parameter_names);
    std::vector<std::string> time_names = {"initial_time", "final_time"};
    updateMaxNameLength(time_names);

    stream << "\nActive or violated parameter bounds" << std::endl;
    stream << "L and U indicate which bound is active; "
              "'*' indicates a bound is violated. "
           << std::endl;
    stream << "The case of lower==upper==value is ignored." << std::endl;

    auto printParameterBounds = [&maxNameLength](const std::string& description,
                                        const std::vector<std::string>& names,
                                        const casadi::DM& values,
                                        const casadi::DM& lower,
                                        const casadi::DM& upper) {
        stream << "\n" << description << ": ";

        bool boundsActive = false;
        bool boundsViolated = false;
        for (casadi_int ivar = 0; ivar < values.rows(); ++ivar) {
            const auto& L = lower(ivar).scalar();
            const auto& V = values(ivar).scalar();
            const auto& U = upper(ivar).scalar();
            if (V <= L || V >= U) {
                if (V == L && L == U) continue;
                boundsActive = true;
                if (V < L || V > U) {
                    boundsViolated = true;
                    break;
                }
            }
        }

        if (!boundsActive && !boundsViolated) {
            stream << "no bounds active or violated" << std::endl;
            return;
        }

        if (!boundsViolated) {
            stream << "some bounds active but no bounds violated";
        } else {
            stream << "some bounds active or violated";
        }

        stream << "\n"
               << std::setw(maxNameLength) << "  " << std::setw(9) << "lower"
               << "    " << std::setw(9) << "value"
               << "    " << std::setw(9) << "upper"
               << " " << std::endl;

        for (casadi_int ivar = 0; ivar < values.rows(); ++ivar) {
            const auto& L = lower(ivar).scalar();
            const auto& V = values(ivar).scalar();
            const auto& U = upper(ivar).scalar();
            if (V <= L || V >= U) {
                // In the case where lower==upper==value, there is no
                // issue; ignore.
                if (V == L && L == U) continue;
                stream << std::setw(maxNameLength) << names[ivar] << "  "
                       << std::setprecision(2) << std::scientific
                       << std::setw(9) << L << " <= " << std::setw(9) << V
                       << " <= " << std::setw(9) << U << " ";
                // Show if the constraint is violated.
                if (V <= L)
                    stream << "L";
                else
                    stream << " ";
                if (V >= U)
                    stream << "U";
                else
                    stream << " ";
                if (V < L || V > U) stream << "*";
                stream << std::endl;
            }
        }
    };
    casadi::DM timeValues(2, 1);
    timeValues(0) = vars.at(initial_time);
    timeValues(1) = vars.at(final_time);

    casadi::DM timeLower(2, 1);
    timeLower(0) = lower.at(initial_time);
    timeLower(1) = lower.at(final_time);

    casadi::DM timeUpper(2, 1);
    timeUpper(0) = upper.at(initial_time);
    timeUpper(1) = upper.at(final_time);

    printParameterBounds(
            "Time bounds", time_names, timeValues, timeLower, timeUpper);
    printParameterBounds("Parameter bounds", it.parameter_names,
            vars.at(parameters), lower.at(parameters), upper.at(parameters));

    // Constraints.
    // ============
    stream << "\nTotal number of constraints: " << m_numConstraints << "."
           << std::endl;

    // Differential equation defects.
    // ------------------------------
    stream << "\nDifferential equation defects:"
           << "\n  L2 norm across mesh, max abs value (L1 norm), time of max "
              "abs"
           << std::endl;

    auto calcL1Norm = [](const casadi::DM& v, int& argmax) {
        double max = v(0).scalar();
        argmax = 0;
        for (int i = 1; i < v.numel(); ++i) {
            if (v(i).scalar() > max) {
                max = std::abs(v(i).scalar());
                argmax = i;
            }
        }
        return max;
    };

    std::string spacer(7, ' ');
    casadi::DM row(1, constraints.defects.columns());
    for (size_t istate = 0; istate < it.state_names.size(); ++istate) {
        row = constraints.defects(istate, Slice());
        const double L2 = casadi::DM::norm_2(row).scalar();
        int argmax;
        double max = calcL1Norm(row, argmax);
        const double L1 = max;
        const double time_of_max = it.times(argmax).scalar();

        stream << std::setw(maxNameLength) << it.state_names[istate] << spacer
               << std::setprecision(2) << std::scientific << std::setw(9) << L2
               << spacer << L1 << spacer << std::setprecision(6) << std::fixed
               << time_of_max << std::endl;
    }

    // Kinematic constraints.
    // ----------------------
    stream << "\nKinematic constraints:";
    std::vector<std::string> kinconNames;
    // TODO: Give better names to kinematic constraints, rather than using
    // the multiplier names.
    for (const auto& kc : m_problem.getMultiplierInfos()) {
        kinconNames.push_back(kc.name);
    }
    if (kinconNames.empty()) { stream << " none" << std::endl; }

    maxNameLength = 0;
    updateMaxNameLength(kinconNames);
    stream << "\n  L2 norm across mesh, max abs value (L1 norm), time of max "
              "abs"
           << std::endl;
    row.resize(1, m_numMeshPoints);
    {
        for (int ikc = 0; ikc < (int)constraints.kinematic.rows(); ++ikc) {
            row = constraints.kinematic(ikc, Slice());
            const double L2 = casadi::DM::norm_2(row).scalar();
            int argmax;
            double max = calcL1Norm(row, argmax);
            const double L1 = max;
            const double time_of_max = it.times(argmax).scalar();

            std::string label = kinconNames[ikc];
            std::cout << std::setfill('0') << std::setw(2) << ikc << ":"
                      << std::setfill(' ') << std::setw(maxNameLength) << label
                      << spacer << std::setprecision(2) << std::scientific
                      << std::setw(9) << L2 << spacer << L1 << spacer
                      << std::setprecision(6) << std::fixed << time_of_max
                      << std::endl;
        }
    }
    stream << "Kinematic constraint values at each mesh point:" << std::endl;
    stream << "      time  ";
    for (int ipc = 0; ipc < (int)kinconNames.size(); ++ipc) {
        stream << std::setw(9) << ipc << "  ";
    }
    stream << std::endl;
    for (int imesh = 0; imesh < m_numMeshPoints; ++imesh) {
        stream << std::setfill('0') << std::setw(3) << imesh << "  ";
        stream.fill(' ');
        stream << std::setw(9) << it.times(imesh).scalar() << "  ";
        for (int ikc = 0; ikc < (int)kinconNames.size(); ++ikc) {
            const auto& value = constraints.kinematic(ikc, imesh).scalar();
            stream << std::setprecision(2) << std::scientific << std::setw(9)
                   << value << "  ";
        }
        stream << std::endl;
    }

    // Path constraints.
    // -----------------
    stream << "\nPath constraints:";
    std::vector<std::string> pathconNames;
    for (const auto& pc : m_problem.getPathConstraintInfos()) {
        pathconNames.push_back(pc.name);
    }

    if (pathconNames.empty()) {
        stream << " none" << std::endl;
        // Return early if there are no path constraints.
        return;
    }
    stream << std::endl;

    maxNameLength = 0;
    updateMaxNameLength(pathconNames);
    // To make space for indices.
    maxNameLength += 3;
    stream << "\n  L2 norm across mesh, max abs value (L1 norm), time of max "
              "abs"
           << std::endl;
    row.resize(1, m_numMeshPoints);
    {
        int ipc = 0;
        for (const auto& pc : m_problem.getPathConstraintInfos()) {
            for (int ieq = 0; ieq < pc.size(); ++ieq) {
                row = constraints.path[ipc](ieq, Slice());
                const double L2 = casadi::DM::norm_2(row).scalar();
                int argmax;
                double max = calcL1Norm(row, argmax);
                const double L1 = max;
                const double time_of_max = it.times(argmax).scalar();

                std::string label = OpenSim::format("%s_%02i", pc.name, ieq);
                std::cout << std::setfill('0') << std::setw(2) << ipc << ":"
                          << std::setfill(' ') << std::setw(maxNameLength)
                          << label << spacer << std::setprecision(2)
                          << std::scientific << std::setw(9) << L2 << spacer
                          << L1 << spacer << std::setprecision(6) << std::fixed
                          << time_of_max << std::endl;
            }
            ++ipc;
        }
    }
    stream << "Path constraint values at each mesh point:" << std::endl;
    stream << "      time  ";
    for (int ipc = 0; ipc < (int)pathconNames.size(); ++ipc) {
        stream << std::setw(9) << ipc << "  ";
    }
    stream << std::endl;
    for (int imesh = 0; imesh < m_numMeshPoints; ++imesh) {
        stream << std::setfill('0') << std::setw(3) << imesh << "  ";
        stream.fill(' ');
        stream << std::setw(9) << it.times(imesh).scalar() << "  ";
        for (int ipc = 0; ipc < (int)pathconNames.size(); ++ipc) {
            const auto& value = constraints.path[ipc](imesh).scalar();
            stream << std::setprecision(2) << std::scientific << std::setw(9)
                   << value << "  ";
        }
        stream << std::endl;
    }
}

Iterate Transcription::createInitialGuessFromBounds() const {
    auto setToMidpoint = [](DM& output, const DM& lowerDM, const DM& upperDM) {
        for (int irow = 0; irow < output.rows(); ++irow) {
            for (int icol = 0; icol < output.columns(); ++icol) {
                const auto& lower = double(lowerDM(irow, icol));
                const auto& upper = double(upperDM(irow, icol));
                if (!std::isinf(lower) && !std::isinf(upper)) {
                    output(irow, icol) = 0.5 * (upper + lower);
                } else if (!std::isinf(lower))
                    output(irow, icol) = lower;
                else if (!std::isinf(upper))
                    output(irow, icol) = upper;
                else
                    output(irow, icol) = 0;
            }
        }
    };
    Iterate casGuess = m_problem.createIterate();
    casGuess.variables = m_lowerBounds;
    for (auto& kv : casGuess.variables) {
        setToMidpoint(kv.second, m_lowerBounds.at(kv.first),
                m_upperBounds.at(kv.first));
    }
    casGuess.times = createTimes(
            casGuess.variables[initial_time], casGuess.variables[final_time]);
    return casGuess;
}

Iterate Transcription::createRandomIterateWithinBounds(
        const SimTK::Random* randGen) const {
    static const SimTK::Random::Uniform randGenDefault(-1, 1);
    const SimTK::Random* randGenToUse = &randGenDefault;
    if (randGen) randGenToUse = randGen;
    auto setRandom = [&](DM& output, const DM& lowerDM, const DM& upperDM) {
        for (int irow = 0; irow < output.rows(); ++irow) {
            for (int icol = 0; icol < output.columns(); ++icol) {
                const auto& lower = double(lowerDM(irow, icol));
                const auto& upper = double(upperDM(irow, icol));
                const auto rand = randGenToUse->getValue();
                auto value = 0.5 * (rand + 1.0) * (upper - lower) + lower;
                if (std::isnan(value)) value = SimTK::clamp(lower, rand, upper);
                output(irow, icol) = value;
            }
        }
    };
    Iterate casIterate = m_problem.createIterate();
    casIterate.variables = m_lowerBounds;
    for (auto& kv : casIterate.variables) {
        setRandom(kv.second, m_lowerBounds.at(kv.first),
                m_upperBounds.at(kv.first));
    }
    casIterate.times = createTimes(casIterate.variables[initial_time],
            casIterate.variables[final_time]);
    return casIterate;
}

casadi::MXVector Transcription::evalOnTrajectory(
        const casadi::Function& pointFunction, const std::vector<Var>& inputs,
        const casadi::Matrix<casadi_int>& timeIndices) const {
    auto parallelism = m_solver.getParallelism();
    const auto trajFunc = pointFunction.map(
            timeIndices.size2(), parallelism.first, parallelism.second);

    // Assemble input.
    // Add 1 for time input and 1 for parameters input.
    MXVector mxIn(inputs.size() + 2);
    mxIn[0] = m_times(timeIndices);
    for (int i = 0; i < (int)inputs.size(); ++i) {
        if (inputs[i] == multibody_states) {
            const auto NQ = m_problem.getNumCoordinates();
            const auto NU = m_problem.getNumSpeeds();
            mxIn[i + 1] = m_vars.at(states)(Slice(0, NQ + NU), timeIndices);
        } else if (inputs[i] == slacks) {
            mxIn[i + 1] = m_vars.at(inputs[i]);
        } else {
            mxIn[i + 1] = m_vars.at(inputs[i])(Slice(), timeIndices);
        }
    }
    if (&timeIndices == &m_gridIndices) {
        mxIn[mxIn.size() - 1] = m_paramsTrajGrid;
    } else if (&timeIndices == &m_daeIndices) {
        mxIn[mxIn.size() - 1] = m_paramsTraj;
    } else if (&timeIndices == &m_daeIndicesIgnoringConstraints) {
        mxIn[mxIn.size() - 1] = m_paramsTrajIgnoringConstraints;
    } else {
        OPENSIM_THROW(OpenSim::Exception, "Internal error.");
    }
    MXVector mxOut;
    trajFunc.call(mxIn, mxOut);
    return mxOut;
    // TODO: Avoid the overhead of map() if not running in parallel.
    /* } else {
    casadi::MXVector out(pointFunction.n_out());
    for (int iout = 0; iout < (int)out.size(); ++iout) {
    out[iout] = casadi::MX(pointFunction.sparsity_out(iout).rows(),
    timeIndices.size2());
    }
    for (int itime = 0; itime < timeIndices.size2(); ++itime) {

    }
    }*/
}

} // namespace CasOC
