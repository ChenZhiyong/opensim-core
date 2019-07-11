#ifndef MOCO_POLYNOMIALACTUATORS_H
#define MOCO_POLYNOMIALACTUATORS_H
/* -------------------------------------------------------------------------- *
 * OpenSim Moco: PolynomialActuators.h                                        *
 * -------------------------------------------------------------------------- *
 * Copyright (c) 2017-19 Stanford University and the Authors                  *
 *                                                                            *
 * Author(s): Antoine Falisse                                                 *
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

#include "../MocoUtilities.h"
#include "../osimMocoDLL.h"

#include "MultivariatePolynomialFunction.h"
#include <OpenSim/Simulation/Model/GeometryPath.h>

namespace OpenSim {

class OSIMMOCO_API PolynomialActuators : public GeometryPath {
    OpenSim_DECLARE_CONCRETE_OBJECT(PolynomialActuators, GeometryPath);
public:
    //=========================================================================
    // PROPERTIES
    //=========================================================================
    OpenSim_DECLARE_PROPERTY(function, Function,
            "The function approximating the geometry: MultivariatePolynomial "
            "or spline (TODO not supported yet)");

    OpenSim_DECLARE_LIST_PROPERTY(coordinate_list, std::string,
        "List containing the generalized coordinates (q's) that parameterize "
        "the function.");

    //=========================================================================
    // METHODS
    //=========================================================================
    PolynomialActuators();

    // Length and Speed of actuator
    double getLength(const SimTK::State& s) const;
    double getLengtheningSpeed(const SimTK::State& s) const;

    // TODO
    /// Add in the equivalent body and generalized forces to be applied to the
    /// multibody system resulting from a tension along the GeometryPath
    /// @param state    state used to evaluate forces
    /// @param[in]  tension      scalar (double) of the applied (+ve) tensile force
    /// @param[in,out] bodyForces   Vector of SpatialVec's (torque, force) on bodies
    /// @param[in,out] mobilityForces  Vector of generalized forces, one per mobility
    void addInEquivalentForces(const SimTK::State& state,
                               const double& tension,
                               SimTK::Vector_<SimTK::SpatialVec>& bodyForces,
                               SimTK::Vector& mobilityForces) const;

private:
    void constructProperties();
    void extendConnectToModel(Model& model) override;
    std::vector<SimTK::ReferencePtr<const Coordinate>> coordinates;

//=============================================================================
};  // END of class PolynomialActuators
//=============================================================================
//=============================================================================

} // namespace OpenSim

#endif // MOCO_POLYNOMIALACTUATORS_H
