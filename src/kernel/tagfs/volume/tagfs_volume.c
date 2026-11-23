#include "tagfs_volume.h"

void TagFSVolumeBegin(TagFSVolume* vol)
{
    memset(vol, 0, sizeof(*vol));
    MountVolumeBegin(vol);
    FallowVolumeBegin(vol);
    CommonholdVolumeBegin(vol);
    ReadAheadVolumeBegin(vol);
    ShowVolumeBegin(vol);
    EndpaperFactVolumeBegin(vol);
    SettleVolumeBegin(vol);
    BinderGleanVolumeBegin(vol);
    CadenceVolumeBegin(vol);
    DogEarVolumeBegin(vol);
    SettleWaitVolumeBegin(vol);
    AwayVolumeBegin(vol);
    TallyReadVolumeBegin(vol);
    IntegrityRotVolumeBegin(vol);
    GlossVolumeBegin(vol);
    HelmVolumeBegin(vol);
    EngrossVolumeBegin(vol);
    DedupGleanVolumeBegin(vol);
}
