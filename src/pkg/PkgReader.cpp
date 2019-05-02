/*
 *   File name: PkgReader.cpp
 *   Summary:	Support classes for QDirStat
 *   License:	GPL V2 - See file LICENSE for details.
 *
 *   Author:	Stefan Hundhammer <Stefan.Hundhammer@gmx.de>
 */

#include "PkgReader.h"
#include "PkgQuery.h"
#include "PkgManager.h"
#include "DirTree.h"
#include "DirReadJob.h"         // LocalDirReadJob
#include "ProcessStarter.h"
#include "Settings.h"
#include "Logger.h"
#include "Exception.h"


using namespace QDirStat;


PkgReader::PkgReader( DirTree * tree ):
    _tree( tree ),
    _maxParallelProcesses( 6 )
{
    logInfo() << endl;
    readSettings();
}


PkgReader::~PkgReader()
{
    writeSettings();

    // Intentionally NOT deleting the PkgInfo * items of _pkgList:
    // They are now owned by the DirTree.
}


void PkgReader::read( const PkgFilter & filter )
{
    logInfo() << endl;

    _pkgList = PkgQuery::installedPkg();
    filterPkgList( filter );
    handleMultiPkg();
    addPkgToTree();
    createReadJobs();

    // Ownership of the PkgInfo * items in _pkgList was transferred to the
    // tree, so intentionally NOT calling qDeleteItems( _pkgList ) !

    _pkgList.clear();
    _multiPkg.clear();
}


void PkgReader::filterPkgList( const PkgFilter & filter )
{
    if ( filter.filterMode() == PkgFilter::SelectAll )
        return;

    PkgInfoList matches;

    foreach ( PkgInfo * pkg, _pkgList )
    {
        if ( filter.matches( pkg->baseName() ) )
        {
            // logDebug() << "Selecting pkg " << pkg << endl;
            matches << pkg;
        }
    }

    _pkgList = matches;
}


void PkgReader::handleMultiPkg()
{
    _multiPkg.clear();

    foreach ( PkgInfo * pkg, _pkgList )
    {
        _multiPkg.insert( pkg->baseName(), pkg );
    }

    foreach ( const QString & pkgName, _multiPkg.uniqueKeys() )
    {
        createDisplayName( pkgName );
    }
}


void PkgReader::createDisplayName( const QString & pkgName )
{
    PkgInfoList pkgList = _multiPkg.values( pkgName );

    if ( pkgList.size() < 2 )
        return;

    QString version  = pkgList.first()->version();
    QString arch     = pkgList.first()->arch();

    bool sameVersion = true;
    bool sameArch    = true;

    for ( int i = 1; i < pkgList.size(); ++i )
    {
        if ( pkgList.at( i )->version() != version )
            sameVersion = false;

        if ( pkgList.at( i )->arch() != arch )
            sameArch = false;
    }

    if ( ! sameVersion )
    {
        logDebug() << "Found multi version pkg " << pkgName
                   << " same arch: " << sameArch
                   << endl;
    }

    foreach ( PkgInfo * pkg, pkgList )
    {
        QString name = pkgName;

        if ( ! sameVersion )
        {
            name += "-" + pkg->version();
            pkg->setMultiVersion( true );
        }

        if ( ! sameArch )
        {
            name += ":" + pkg->arch();
            pkg->setMultiArch( true );
        }

        // logDebug() << " Setting name " << name << endl;
        pkg->setName( name );
    }
}


void PkgReader::addPkgToTree()
{
    CHECK_PTR( _tree );
    CHECK_PTR( _tree->root() );

    PkgInfo * top = new PkgInfo( _tree, _tree->root(), "Pkg:", 0 );
    CHECK_NEW( top );
    _tree->root()->insertChild( top );

    foreach ( PkgInfo * pkg, _pkgList )
    {
        pkg->setTree( _tree );
        top->insertChild( pkg );
    }

    top->setReadState( DirFinished );
    _tree->sendFinalizeLocal( top );
    top->finalizeLocal();
}


void PkgReader::createReadJobs()
{
    ProcessStarter * processStarter = new ProcessStarter;
    CHECK_NEW( processStarter );
    processStarter->setAutoDelete( true );
    processStarter->setMaxParallel( _maxParallelProcesses );

    foreach ( PkgInfo * pkg, _pkgList )
    {
        Process * process = createReadFileListProcess( pkg );

        if ( process )
        {
            PkgReadJob * job = new PkgReadJob( _tree, pkg, process );
            CHECK_NEW( job );
            _tree->addBlockedJob( job );
            processStarter->add( process );
        }
    }

    processStarter->start();
}


Process * PkgReader::createReadFileListProcess( PkgInfo * pkg )
{
    CHECK_PTR( pkg );
    CHECK_PTR( pkg->pkgManager() );

    QString command = pkg->pkgManager()->fileListCommand( pkg );

    if ( command.isEmpty() )
    {
        logError() << "Empty file list command for " << pkg << endl;
        return 0;
    }

    QStringList args     = command.split( QRegExp( "\\s+" ) );
    QString     program  = args.takeFirst();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert( "LANG", "C" ); // Prevent output in translated languages

    Process * process = new Process();
    process->setProgram( program );
    process->setArguments( args );
    process->setProcessEnvironment( env );
    process->setProcessChannelMode( QProcess::MergedChannels ); // combine stdout and stderr

    // Intentionally NOT starting the process yet

    return process;
}


void PkgReader::readSettings()
{
    Settings settings;
    settings.beginGroup( "Pkg" );

    _maxParallelProcesses = settings.value( "MaxParallelProcesses", 6 ).toInt();

    settings.endGroup();
}


void PkgReader::writeSettings()
{
    Settings settings;
    settings.beginGroup( "Pkg" );

    settings.setValue( "MaxParallelProcesses", _maxParallelProcesses );

    settings.endGroup();
}






PkgReadJob::PkgReadJob( DirTree * tree,
                        PkgInfo * pkg,
                        Process * readFileListProcess ):
    ObjDirReadJob( tree, pkg ),
    _pkg( pkg ),
    _readFileListProcess( readFileListProcess )
{
    if ( _readFileListProcess )
    {
        connect( _readFileListProcess, SIGNAL( finished	           ( int, QProcess::ExitStatus ) ),
                 this,                 SLOT  ( readFileListFinished( int, QProcess::ExitStatus ) ) );
    }
}


PkgReadJob::~PkgReadJob()
{

}


void PkgReadJob::readFileListFinished( int                  exitCode,
                                       QProcess::ExitStatus exitStatus )
{
    CHECK_PTR( _readFileListProcess );
    CHECK_PTR( _pkg );
    CHECK_PTR( _pkg->pkgManager() );

    bool ok        = true;

    if ( exitStatus != QProcess::NormalExit )
    {
        ok = false;
        logError() << "Get file list command crashed for " << _pkg << endl;
    }

    if ( ok && exitCode != 0 )
    {
        ok = false;
        logError() << "Get file list command exited with "
                   << exitStatus << " for " << _pkg << endl;
    }

    if ( ok )
    {
        QString output = QString::fromUtf8( _readFileListProcess->readAll() );
        _fileList      = _pkg->pkgManager()->parseFileList( output );
        _tree->unblock( this ); // schedule this job
        _readFileListProcess->deleteLater();
    }
    else
    {
        _pkg->setReadState( DirError );
        _tree->sendReadJobFinished( _pkg );
        delete _readFileListProcess;
        _readFileListProcess = 0;
        finished();
        // Don't add anything after finished() since this deletes this job!
    }
}


void PkgReadJob::startReading()
{
    // logInfo() << "Reading " << _pkg << endl;
    CHECK_PTR( _pkg );

    _pkg->setReadState( DirReading );

    foreach ( const QString & path, _fileList )
    {
        if ( ! path.isEmpty() )
            addFile( path );
    }

    finalizeAll( _pkg );
    _tree->sendReadJobFinished( _pkg );
    finished();
    // Don't add anything after finished() since this deletes this job!
}


void PkgReadJob::addFile( const QString & fileListPath )
{
    // logDebug() << "Adding " << fileListPath << " to " << _pkg << endl;

    QStringList remaining = fileListPath.split( "/", QString::SkipEmptyParts );
    QStringList currentPath;
    DirInfo *   parent = _pkg;

    while ( ! remaining.isEmpty() )
    {
        QString currentName = remaining.takeFirst();
        currentPath << currentName;
        FileInfo * newParent = _pkg->locate( parent, QStringList() << currentName );

        if ( ! newParent )
        {
            QString path = QString( "/" ) + currentPath.join( "/" );

            newParent = LocalDirReadJob::stat( path, _tree, parent,
                                               false ); // doThrow

            if ( ! newParent )
            {
                parent->setReadState( DirError );
                return;
            }

            // logDebug() << "Created " << newParent << endl;
        }

        if ( ! remaining.isEmpty() )
        {
            parent = newParent->toDirInfo();

            if ( ! parent )
            {
                logWarning() << newParent << " should be a directory, but is not" << endl;
                return;
            }
        }
    }
}


void PkgReadJob::finalizeAll( DirInfo * subtree )
{
    FileInfo * child = subtree->firstChild();

    while( child )
    {
        if ( child->isDirInfo() )
            finalizeAll( child->toDirInfo() );

        child = child->next();
    }

    if ( subtree->readState() != DirError )
        subtree->setReadState( DirFinished );

    _tree->sendFinalizeLocal( subtree );
    subtree->finalizeLocal();
}

