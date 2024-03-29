#include "SPH.h"
#include <cmath>
#include <QDebug>

SPH::SPH( AbstractObject* parent, const Geometry& container, float smoothingRadius, float viscosity, float pressure, float surfaceTension,
          unsigned int nbCellX, unsigned int nbCellY, unsigned int nbCellZ, unsigned int nbCubeX,
          unsigned int nbCubeY, unsigned int nbCubeZ, unsigned int nbParticles, float restDensity,
          float totalVolume, float maxDTime, const QVector3D& gravity )
    : AbstractObject( parent )
    , _container( container )
    , _coeffPoly6( 0 )
    , _coeffSpiky( 0 )
    , _coeffVisc( 0 )
    , _restDensity( restDensity )
    , _smoothingRadius( smoothingRadius )
    , _smoothingRadius2( smoothingRadius * smoothingRadius )
    , _viscosity( viscosity )
    , _pressure( pressure )
    , _surfaceTension( surfaceTension )
    , _maxDeltaTime( maxDTime )
    , _gravity( gravity )
    , _particles( nbParticles )
    , _grid( inflatedContainerBoundingBox(), nbCellX, nbCellY, nbCellZ, smoothingRadius )
    , _marchingTetrahedra( inflatedContainerBoundingBox(), nbCubeX, nbCubeY, nbCubeZ )
    , _renderMode( RenderParticles )
    , _material( QColor( 128, 128, 128, 255 ) )
{
    initializeCoefficients();
    initializeParticles( totalVolume );
}

SPH::~SPH()
{
}

void SPH::animate( const TimeState& timeState )
{
    float deltaTime = timeState.deltaTime();

    // Clamp 'dt' to avoid instabilities
    if ( deltaTime > _maxDeltaTime )
        deltaTime = _maxDeltaTime;

    computeDensities();
    computeForces();
    moveParticles( deltaTime );
}

void SPH::render( GLShader& shader )
{
    shader.setMaterial( _material );

    switch( _renderMode )
    {
    case RenderParticles : _particles.render( globalTransformation(), shader ); break;
    case RenderImplicitSurface : _marchingTetrahedra.render( globalTransformation(), shader, *this ); break;
    }
}

void SPH::changeRenderMode()
{
    if ( _renderMode == RenderParticles )
        _renderMode = RenderImplicitSurface;
    else
        _renderMode = RenderParticles;
}

void SPH::changeMaterial()
{
    if ( _material.refractiveIndex() == 1 )
        _material = Material( 1.33 );
    else
        _material = Material( QColor( 128, 128, 128, 255 ) );
}

void SPH::resetVelocities()
{
    for ( int i=0 ; i<_particles.size() ; ++i )
        _particles[i].setVelocity( QVector3D() );
}

BoundingBox SPH::inflatedContainerBoundingBox() const
{
    BoundingBox boundingBox = _container.boundingBox();
    QVector3D center = ( boundingBox.minimum() + boundingBox.maximum() ) * 0.5;

    return BoundingBox( ( boundingBox.minimum() - center ) * 1.2 + center,
                        ( boundingBox.maximum() - center ) * 1.2 + center );
}

void SPH::initializeCoefficients()
{
    // H^n
    float h6 = _smoothingRadius2 * _smoothingRadius2 * _smoothingRadius2;
    float h9 = h6 * _smoothingRadius2 * _smoothingRadius;

    // Poly6
    _coeffPoly6 = 315.0 / ( 64.0 * M_PI * h9 );

    // Spiky
    _coeffSpiky = 3.0 * 15.0 / ( M_PI * h6 );

    // Visc
    _coeffVisc = 45.0 / ( M_PI * h6 );
}

void SPH::initializeParticles( float totalVolume )
{
    float totalMass = totalVolume * _restDensity;
    float mass = totalMass / _particles.size();

    // set particle position and mass
    for ( int i=0 ; i<_particles.size() ; ++i )
    {
        _particles[i].setMass( mass );
        _particles[i].setDensity( _restDensity );
        _particles[i].setVolume( mass / _restDensity );
        _particles[i].setPosition( _container.randomInteriorPoint() );
        _particles[i].setCellIndex( _grid.cellIndex( _particles[i].position() ) );

        _grid.addParticle( _particles[i].cellIndex(), i );
    }
}

float SPH::densityKernel( float r2 ) const
{
    float diff = _smoothingRadius2 - r2;

    return _coeffPoly6 * diff * diff * diff;
}

float SPH::densitykernelGradient( float r2 ) const
{
    float diff = _smoothingRadius2 - r2;

    return -3 * _coeffPoly6 * diff * diff;
}

float SPH::pressureKernel( float r ) const
{
    if ( r == 0 )
        return 0;

    float diff = _smoothingRadius - r;

    return _coeffSpiky * diff * diff / r;
}

float SPH::viscosityKernel( float r ) const
{
    return _coeffVisc * ( _smoothingRadius - r );
}

float SPH::pressure( float density ) const
{
    return density / _restDensity - 1;
}

void SPH::computeDensities()
{
    // For each particle
#pragma omp parallel for schedule( guided )
    for ( int i=0 ; i<_particles.size() ; ++i )
    {
        float density = 0;
        float correction = 0;
        Particle& particle = _particles[i];
        const QVector<unsigned int>& neighborhood = _grid.neighborhood( particle.cellIndex() );

        // For each neighbor cell
        for ( int j=0 ; j<neighborhood.size() ; ++j )
        {
            const QVector<unsigned int>& neighbors = _grid.cellParticles( neighborhood[j] );

            // For each particle in a neighboring cell
            for ( int k=0 ; k<neighbors.size() ; ++k )
            {
                const Particle& neighbor = _particles[neighbors[k]];
                QVector3D difference = particle.position() - neighbor.position();
                float r2 = difference.lengthSquared();

                // If the neighboring particle is inside a sphere of radius 'h'
                if ( r2 < _smoothingRadius2 )
                {
                    // Add density contribution
                    float kernelMass = densityKernel( r2 ) * neighbor.mass();
                    density += kernelMass;
                    correction += kernelMass / neighbor.density();
                }
            }
        }

        particle.setDensity( density / correction );
        particle.setVolume( particle.mass() / particle.density() );
        particle.setPressure( pressure( density ) );
    }
}

void SPH::computeForces()
{
    // Compute gravity vector
    QVector3D gravity = localTransformation().inverted().mapVector( _gravity );

    // For each particle
#pragma omp parallel for schedule( guided )
    for ( int i=0 ; i<_particles.size() ; ++i )
    {
        QVector3D pressureForce;
        QVector3D viscosityForce;
        QVector3D tensionForce;
        float correction = 0;

        Particle& particle = _particles[i];
        const QVector<unsigned int>& neighborhood = _grid.neighborhood( particle.cellIndex() );

        // For each neighbor cell
        for ( int j=0 ; j<neighborhood.size() ; ++j )
        {
            const QVector<unsigned int>& neighbors = _grid.cellParticles( neighborhood[j] );

            // For each particle in a neighboring cell
            for ( int k=0 ; k<neighbors.size() ; ++k )
            {
                const Particle& neighbor = _particles[neighbors[k]];
                QVector3D difference = particle.position() - neighbor.position();
                float r2 = difference.lengthSquared();

                // If the neighboring particle is inside a sphere of radius 'h'
                if ( r2 < _smoothingRadius2 )
                {
                    float r = ::sqrt( r2 );
                    float volume = neighbor.volume();
                    float meanPressure = ( neighbor.pressure() + particle.pressure() ) * 0.5;

                    // Add forces contribution
                    pressureForce -= difference * ( pressureKernel( r ) * meanPressure * volume );
                    viscosityForce += ( neighbor.velocity() - particle.velocity() ) * ( viscosityKernel( r ) * volume );

                    float kernelRR = densityKernel( r2 );
                    tensionForce += difference * kernelRR; // * Mass_b / Mass_a, but in our case, this equals 1
                    correction += kernelRR * volume;
                }
            }
        }

        // Normalize results and apply uniform coefficients;
        pressureForce *= _pressure / correction;
        viscosityForce *= _viscosity / correction;
        tensionForce *= _surfaceTension / correction;

        // Compute the sum of all forces and convert it to an acceleration
        particle.setAcceleration( ( viscosityForce - pressureForce - tensionForce ) / particle.density() + gravity );
    }
}

void SPH::moveParticles( float deltaTime )
{

    ////////////////////////////////////////////////////
    // IFT3355 - À compléter
    //
    // Pour chaque particule, utilisez son accélération
    // précédemment obtenue pour calculer sa nouvelle
    // vélocité puis sa nouvelle position à l'aide
    // de la méthode d'intégration semi-explite d'Euler.
    //
    // Vous devrez aussi vous assurer qu'il n'y a pas
    // d'intersection avec la paroi (_container) du
    // contenant. S'il y a intersection, corriger la
    // vélocité et la position en conséquence.
    //
    // Une fois que la particule a atteint sa destination
    // finale, vérifiez si la particule a changé de
    // cellule de la grille régulière à l'aide de la
    // classe 'Grid'. Si oui, changez-la de cellule avec
    // les méthodes 'removeParticle' et 'addParticle' et
    // mettez à jour son index.
    ////////////////////////////////////////////////////

    float bias = 0.0005;
    for (int i = 0; i < _particles.size(); i++)
    {
        // Calcul de la nouvelle velocite
        // Methode d'Euler semi-explicite
        QVector3D velocity = _particles[i].velocity() + deltaTime * _particles[i].acceleration();

        // Calcul de la nouvelle position, du mouvement et initialisation du mouvement restant
        QVector3D position = _particles[i].position();
        QVector3D newPosition = _particles[i].position() + deltaTime * velocity;
        QVector3D movement = newPosition - position;
        QVector3D movementLeft = movement;

        //Initialisation de l'intersection
        Intersection intersection;

        // Boucle tant qu'il y a des intersections
        while (true)
        {
            QVector3D direction = movement.normalized();
            Ray ray = Ray(position, direction);

            // Si la particule intersecte le container avant d'avoir atteint sa position finale
            if (_container.intersect(ray, intersection) &&
                    (intersection.rayParameterT() * direction).length() < movementLeft.length())
            {

                QVector3D normal = intersection.normal();

                position = intersection.position();

                // Calcul du mouvement restant que la collision a empechee
                movementLeft = newPosition - position;

                //Calcul du nouveau mouvement par une projection normalisee a laquelle on multiplie la longueur
                //du mouvement restant, (collision non elastique)
                movement = movementLeft - QVector3D::dotProduct(movementLeft, normal) * normal;

                // Positionnement de la particule juste un peu avant l'intersection rencontree, pour que
                // la particule ne soit pas directement sur la surface du container lors du prochain trace
                position = position - normal*bias;

                //Calcul de la nouvelle position
                newPosition = position + movement;

                // Correction de la velocite
                velocity = velocity - QVector3D::dotProduct(velocity, normal) * normal;
            }

            // Si la particule a atteint sa position finale (ie plus d'intersection)
            else
            {

                position += movement;
                _particles[i].setPosition(position);
                _particles[i].setVelocity(velocity);
                break;
            }
        }

        // Mise a jour de la cellule dans la grille
        unsigned int oldCellIndex = _particles[i].cellIndex();
        unsigned int newCellIndex = _grid.cellIndex(position);
        if (oldCellIndex != newCellIndex)
        {
            _particles[i].setCellIndex(newCellIndex);
            _grid.removeParticle(oldCellIndex, i);
            _grid.addParticle(newCellIndex, i);
        }
    }


}


void SPH::surfaceInfo( const QVector3D& position, float& value, QVector3D& normal )
{
    ////////////////////////////////////////////////////
    // IFT3355 - À compléter
    //
    // Vous devez calculer la valeur de la fonction 'f'
    // et l'approxmation de la normale à la surface au
    // point 'position'. Cette fonction est appelée
    // par la classe 'MarchingTetrahedra' à chaque sommet
    // de sa grille.
    //
    // Basez-vous sur les calculs tels que 'computeDensities'
    // et 'computeForces' pour savoir comment accéder aux
    // particules voisines.
    ////////////////////////////////////////////////////

    //Initialisation des variables de densite
    float density = 0;
    float densityKernelGradientX = 0;
    float densityKernelGradientY = 0;
    float densityKernelGradientZ = 0;

    //Get neighborhood
    const QVector<unsigned int>& neighborhood = _grid.neighborhood(_grid.cellIndex(position));

    // For each neighbor cell
    for ( int j=0 ; j<neighborhood.size() ; ++j )
    {
        //Get particles of the cell
        const QVector<unsigned int>& neighbors = _grid.cellParticles( neighborhood[j] );

        // For each particle in a neighboring cell
        for ( int k=0 ; k<neighbors.size() ; ++k )
        {
            const Particle& neighbor = _particles[neighbors[k]];
            QVector3D difference = position - neighbor.position();
            float r2 = difference.lengthSquared();

            // If the neighboring particle is inside a sphere of radius 'h'
            if ( r2 < _smoothingRadius2 )
            {
                // Add density contribution
                density += densityKernel( r2 ) * neighbor.mass();

                // Calcul des composantes du gradient de f
                densityKernelGradientX +=(2*(position.x() - neighbor.position().x()))* densitykernelGradient(r2)*neighbor.mass();
                densityKernelGradientY +=(2*(position.y() - neighbor.position().y()))* densitykernelGradient(r2)*neighbor.mass();
                densityKernelGradientZ +=(2*(position.z() - neighbor.position().z()))*densitykernelGradient(r2)*neighbor.mass();

            }
        }
    }

    normal.setX(densityKernelGradientX/_restDensity); //Etant donne que cest une derivee on ne rajoute pas le "-(1-a)"
    normal.setY(densityKernelGradientY/_restDensity);
    normal.setZ(densityKernelGradientZ/_restDensity);

    normal.normalize();
    normal = -normal;
    float a = 0.3;
    value = density / _restDensity - (1 - a);
}
