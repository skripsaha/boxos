#include "tagfs_volume.h"

/*
 * The home volume: the one this kernel was booted from, as the loader's pass
 * names it. Until a volume is taken up beside it, it is the only one.
 */
static TagFSVolume s_home;

TagFSVolume* TagFSHome(void)
{
    return &s_home;
}

void TagFSHomeBegin(void)
{
    TagFSVolumeBegin(&s_home);
}

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
