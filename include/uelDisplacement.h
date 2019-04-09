#pragma once
#include "bftConstants.h"
#include "bftElement.h"
#include "bftElementProperty.h"
#include "bftFiniteElement.h"
#include "bftFunctions.h"
#include "bftGeometryElement.h"
#include "bftMaterialHypoElastic.h"
#include "bftMath.h"
#include "bftTypedefs.h"
#include "bftVoigt.h"
#include "userLibrary.h"
#include <iostream>
#include <memory>
#include <vector>

using namespace bft;
using namespace Eigen;

template <int nDim, int nNodes>
class UelDisplacement : public BftElement, public BftGeometryElement<nDim, nNodes> {

  public:
    enum SectionType {
        UniaxialStress,
        PlaneStress,
        PlaneStrain,
        Solid,
    };

    static constexpr int sizeLoadVector = nNodes * nDim;
    static constexpr int nCoordinates   = nNodes * nDim;

    using ParentGeometryElement = BftGeometryElement<nDim, nNodes>;
    using JacobianSized         = typename ParentGeometryElement::JacobianSized;
    using dNdXiSized            = typename ParentGeometryElement::dNdXiSized;
    using BSized                = typename ParentGeometryElement::BSized;
    using XiSized               = typename ParentGeometryElement::XiSized;
    using RhsSized              = Matrix<double, sizeLoadVector, 1>;
    using KeSizedMatrix         = Matrix<double, sizeLoadVector, sizeLoadVector>;
    using CSized                = Matrix<double, ParentGeometryElement::VoigtSize, ParentGeometryElement::VoigtSize>;
    using Voigt                 = Matrix<double, ParentGeometryElement::VoigtSize, 1>;

    Map<const VectorXd> elementProperties;
    const int           elLabel;
    const SectionType   sectionType;

    struct GaussPt {
        static constexpr int nRequiredStateVars = 6 + 6;

        const XiSized xi;
        const double  weight;

        std::unique_ptr<BftMaterialHypoElastic> material;
        mVector6                                stress;
        mVector6                                strain;

        struct Geometry {
            JacobianSized J;
            JacobianSized JInv;
            double        detJ;
            dNdXiSized    dNdXi;
            dNdXiSized    dNdX;
            BSized        B;
            double        intVol;
        };

        std::unique_ptr<Geometry> geometry;

        GaussPt( XiSized xi, double weight ) : xi( xi ), weight( weight ), stress( nullptr ), strain( nullptr ){};
    };

    std::vector<GaussPt> gaussPts;

    UelDisplacement( int noEl, NumIntegration::IntegrationTypes integrationType, SectionType sectionType );

    int getNumberOfRequiredStateVars();

    std::vector<std::vector<std::string>> getNodeFields();

    std::vector<int> getDofIndicesPermutationPattern();

    int getNNodes() { return nNodes; }

    int getNDofPerElement() { return sizeLoadVector; }

    std::string getElementShape() { return ParentGeometryElement::getElementShape(); }

    void assignStateVars( double* stateVars, int nStateVars );

    void assignProperty( const ElementProperties& bftElementProperty );

    void assignProperty( const BftMaterialSection& bftElementProperty );

    void initializeYourself( const double* coordinates );

    void setInitialConditions( StateTypes state, const double* values );

    void computeDistributedLoad( BftElement::DistributedLoadTypes loadType,
                                 double*                          P,
                                 double*                          K,
                                 const int                        elementFace,
                                 const double*                    load,
                                 const double*                    QTotal,
                                 const double*                    time,
                                 double                           dT );

    void computeBodyForce( double*       P,
                           double*       K,
                           const double* load,
                           const double* QTotal,
                           const double* time,
                           double        dT );

    void computeYourself( const double* QTotal,
                          const double* dQ,
                          double*       Pe,
                          double*       Ke,
                          const double* time,
                          double        dT,
                          double&       pNewdT );

    double* getPermanentResultPointer( const std::string& resultName, int gaussPt, int& resultLength )
    {
        if ( resultName == "stress" ) {
            resultLength = Vgt::VoigtSize;
            return gaussPts[gaussPt].stress.data();
        }
        else if ( resultName == "strain" ) {
            resultLength = Vgt::VoigtSize;
            return gaussPts[gaussPt].strain.data();
        }
        else if ( resultName == "sdv" ) {
            resultLength = gaussPts[gaussPt].material->nStateVars;
            return gaussPts[gaussPt].material->stateVars;
        }
        else
            return this->gaussPts[gaussPt].material->getPermanentResultPointer( resultName, resultLength );
    }
};

template <int nDim, int nNodes>
UelDisplacement<nDim, nNodes>::UelDisplacement( int                              noEl,
                                                NumIntegration::IntegrationTypes integrationType,
                                                SectionType                      sectionType )
    : ParentGeometryElement(),
      elementProperties( Map<const VectorXd>( nullptr, 0 ) ),
      elLabel( noEl ),
      sectionType( sectionType )
{
    for ( const auto& gaussPtInfo : NumIntegration::getGaussPointInfo( this->shape, integrationType ) ) {
        GaussPt gpt( gaussPtInfo.xi, gaussPtInfo.weight );
        gaussPts.push_back( std::move( gpt ) );
    }
}

template <int nDim, int nNodes>
int UelDisplacement<nDim, nNodes>::getNumberOfRequiredStateVars()
{
    return ( gaussPts[0].material->getNumberOfRequiredStateVars() + GaussPt::nRequiredStateVars ) * gaussPts.size();
}

template <int nDim, int nNodes>
std::vector<std::vector<std::string>> UelDisplacement<nDim, nNodes>::getNodeFields()
{
    using namespace std;

    static vector<vector<string>> nodeFields;
    if ( nodeFields.empty() )
        for ( int i = 0; i < nNodes; i++ ) {
            nodeFields.push_back( vector<string>() );
            nodeFields[i].push_back( "displacement" );
        }

    return nodeFields;
}

template <int nDim, int nNodes>
std::vector<int> UelDisplacement<nDim, nNodes>::getDofIndicesPermutationPattern()
{
    static std::vector<int> permutationPattern;
    if ( permutationPattern.empty() )
        for ( int i = 0; i < nNodes * nDim; i++ )
            permutationPattern.push_back( i );

    return permutationPattern;
}

template <int nDim, int nNodes>
void UelDisplacement<nDim, nNodes>::assignStateVars( double* stateVars, int nStateVars )
{
    /* we provide as many statevars to the material as we can (some materials store addition debugging infos if possible
     * alternatively:
     * int nStateVarsMaterial = gaussPts[0].material->getNumberOfRequiredStateVars();
     * */
    int nStateVarsMaterial = ( nStateVars / gaussPts.size() ) - GaussPt::nRequiredStateVars;

    for ( size_t i = 0; i < gaussPts.size(); i++ ) {

        GaussPt& gpt = gaussPts[i];
        gpt.material->assignStateVars( stateVars + i * ( nStateVarsMaterial + GaussPt::nRequiredStateVars ),
                                       nStateVarsMaterial );

        // assign stress, strain state vars using the 'placement new' operator
        new ( &gpt.stress )
            mVector6( stateVars + nStateVarsMaterial + i * ( nStateVarsMaterial + GaussPt::nRequiredStateVars ), 6 );

        new ( &gpt.strain )
            mVector6( stateVars + nStateVarsMaterial + i * ( nStateVarsMaterial + GaussPt::nRequiredStateVars ) + 6,
                      6 );
    }
}

template <int nDim, int nNodes>
void UelDisplacement<nDim, nNodes>::assignProperty( const ElementProperties& elementPropertiesInfo )
{
    new ( &elementProperties ) Eigen::Map<const Eigen::VectorXd>( elementPropertiesInfo.elementProperties,
                                                                  elementPropertiesInfo.nElementProperties );
}

template <int nDim, int nNodes>
void UelDisplacement<nDim, nNodes>::assignProperty( const BftMaterialSection& section )
{
    for ( size_t i = 0; i < gaussPts.size(); i++ ) {
        GaussPt& gpt = gaussPts[i];
        gpt.material = std::unique_ptr<BftMaterialHypoElastic>(
            dynamic_cast<BftMaterialHypoElastic*>( userLibrary::bftMaterialFactory( section.materialCode,
                                                                                    section.materialProperties,
                                                                                    section.nMaterialProperties,
                                                                                    elLabel,
                                                                                    i ) ) );
    }
}

template <int nDim, int nNodes>
void UelDisplacement<nDim, nNodes>::initializeYourself( const double* coordinates )
{
    ParentGeometryElement::initializeYourself( coordinates );

    for ( GaussPt& gpt : gaussPts ) {

        gpt.geometry = std::make_unique<typename GaussPt::Geometry>();

        gpt.geometry->dNdXi = this->dNdXi( gpt.xi );
        gpt.geometry->J     = this->Jacobian( gpt.geometry->dNdXi );
        gpt.geometry->JInv  = gpt.geometry->J.inverse();
        gpt.geometry->detJ  = gpt.geometry->J.determinant();
        gpt.geometry->dNdX  = this->dNdX( gpt.geometry->dNdXi, gpt.geometry->JInv );
        gpt.geometry->B     = this->B( gpt.geometry->dNdX );

        if ( sectionType == SectionType::Solid ) {

            gpt.geometry->intVol = gpt.weight * gpt.geometry->detJ;
            gpt.material->setCharacteristicElementLength( std::cbrt( 8 * gpt.geometry->detJ ) );
        }

        else if ( sectionType == SectionType::PlaneStrain || sectionType == SectionType::PlaneStress ) {

            const double& thickness = elementProperties[0];
            gpt.geometry->intVol    = gpt.weight * gpt.geometry->detJ * thickness;
            gpt.material->setCharacteristicElementLength( std::sqrt( 4 * gpt.geometry->detJ ) );
        }

        else if ( sectionType == SectionType::UniaxialStress ) {

            const double& crossSection = elementProperties[0];
            gpt.geometry->intVol       = gpt.weight * gpt.geometry->detJ * crossSection;
            gpt.material->setCharacteristicElementLength( 2 * gpt.geometry->detJ );
        }
    }
}

template <int nDim, int nNodes>
void UelDisplacement<nDim, nNodes>::computeYourself( const double* QTotal_,
                                                     const double* dQ_,
                                                     double*       Pe_,
                                                     double*       Ke_,
                                                     const double* time,
                                                     double        dT,
                                                     double&       pNewDT )
{
    using namespace bft;

    Map<const RhsSized> QTotal( QTotal_ );
    Map<const RhsSized> dQ( dQ_ );
    Map<KeSizedMatrix>  Ke( Ke_ );
    Map<RhsSized>       Pe( Pe_ );

    Voigt  S, dE;
    CSized C;

    for ( GaussPt& gaussPt : gaussPts ) {

        const BSized& B = gaussPt.geometry->B;

        dE = B * dQ;

        if constexpr ( nDim == 1 ) {
            Vector6 dE6;
            dE6 << dE, 0, 0, 0, 0, 0;
            Matrix6 C66;
            gaussPt.material->computeUniaxialStress( gaussPt.stress.data(),
                                                     C66.data(),
                                                     // gaussPt.strain.data(),
                                                     dE6.data(),
                                                     time,
                                                     dT,
                                                     pNewDT );

            C << mechanics::getUniaxialStressTangent( C66 );
        }

        else if constexpr ( nDim == 2 ) {

            Vector6 dE6 = Vgt::planeVoigtToVoigt( dE );
            Matrix6 C66;

            if ( sectionType == SectionType::PlaneStress ) {

                gaussPt.material->computePlaneStress( gaussPt.stress.data(),
                                                      C66.data(),
                                                      // gaussPt.strain.data(),
                                                      dE6.data(),
                                                      time,
                                                      dT,
                                                      pNewDT );

                C = mechanics::getPlaneStressTangent( C66 );
            }

            else if ( sectionType == SectionType::PlaneStrain ) {

                gaussPt.material->computeStress( gaussPt.stress.data(),
                                                 C66.data(),
                                                 // gaussPt.strain.data(),
                                                 dE6.data(),
                                                 time,
                                                 dT,
                                                 pNewDT );

                C = mechanics::getPlaneStrainTangent( C66 );
            }

            S = Vgt::voigtToPlaneVoigt( gaussPt.stress );
            gaussPt.strain += dE6;
        }

        else if constexpr ( nDim == 3 ) {

            if ( sectionType == SectionType::Solid ) {

                gaussPt.material->computeStress( gaussPt.stress.data(),
                                                 C.data(),
                                                 // gaussPt.strain.data(),
                                                 dE.data(),
                                                 time,
                                                 dT,
                                                 pNewDT );
            }

            S = gaussPt.stress;
            gaussPt.strain += dE;
        }

        if ( pNewDT < 1.0 )
            return;

        Ke += B.transpose() * C * B * gaussPt.geometry->intVol;
        Pe -= B.transpose() * S * gaussPt.geometry->intVol;
    }
}

template <int nDim, int nNodes>
void UelDisplacement<nDim, nNodes>::setInitialConditions( StateTypes state, const double* values )
{
    switch ( state ) {
    case BftElement::GeostaticStress: {
        if constexpr ( nDim > 1 ) {
            for ( GaussPt& gaussPt : gaussPts ) {

                XiSized coordAtGauss = this->NB( this->N( gaussPt.xi ) ) * this->coordinates;

                const double sigY1 = values[0];
                const double sigY2 = values[2];
                const double y1    = values[1];
                const double y2    = values[3];

                using namespace Math;
                gaussPt.stress( 1 ) = linearInterpolation( coordAtGauss[1], y1, y2, sigY1, sigY2 ); // sigma_y
                gaussPt.stress( 0 ) = values[4] * gaussPt.stress( 1 );                              // sigma_x
                gaussPt.stress( 2 ) = values[5] * gaussPt.stress( 1 );
            } // sigma_z
        }
        break;
    }

    default: break;
    }
}

template <int nDim, int nNodes>
void UelDisplacement<nDim, nNodes>::computeDistributedLoad( BftElement::DistributedLoadTypes loadType,
                                                            double*                          P,
                                                            double*                          K,
                                                            const int                        elementFace,
                                                            const double*                    load,
                                                            const double*                    QTotal,
                                                            const double*                    time,
                                                            double                           dT )
{
    Map<RhsSized> fU( P );

    switch ( loadType ) {

    case BftElement::Pressure: {
        const double p = load[0];

        FiniteElement::BoundaryElement boundaryEl( this->shape, elementFace, nDim, this->coordinates );

        VectorXd Pk = -p * boundaryEl.computeNormalLoadVector();

        if ( nDim == 2 )
            Pk *= elementProperties[0]; // thickness

        boundaryEl.assembleIntoParentVector( Pk, fU );

        break;
    }
    default: {
        throw std::invalid_argument( "Invalid Load Type specified" );
    }
    }
}

template <int nDim, int nNodes>
void UelDisplacement<nDim, nNodes>::computeBodyForce( double*       P_,
                                                      double*       K,
                                                      const double* load,
                                                      const double* QTotal,
                                                      const double* time,
                                                      double        dT )
{
    Map<RhsSized>                            Pe( P_ );
    const Map<const Matrix<double, nDim, 1>> f( load );

    for ( const auto& gpt : gaussPts )
        Pe += this->NB( this->N( gpt.xi ) ).transpose() * f * gpt.geometry->intVol;
}
