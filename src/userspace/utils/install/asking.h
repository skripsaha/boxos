/* asking — the person asked and told in the hub, a step at a time (install.h).
 * The window in Gangway's look asks the same questions; until it stands,
 * this is how install talks. */
#ifndef INSTALL_ASKING_H
#define INSTALL_ASKING_H

#include "install.h"

namespace install {

/* The steps told as they go: each step's name, the files carried by the
 * quarter, a refusal as it came. */
class HubTelling final : public Telling {
public:
    void Began(Step S) override;
    void Carrying(Step S, std::uint32_t Done, std::uint32_t Of) override;
    void Said(std::string_view Line) override;

private:
    std::uint32_t Quarter = 0;
};

/* The machine's disks shown, each with what lies on it. */
void ShowDisks(const Facts &Now);

/* The disk BoxOS is to go on, asked; nullptr when the person gave none. */
const DiskListed *AskDisk(const Facts &Now);
/* Whole disk or beside what is there: asked when the disk holds anything. */
bool AskWhere(const DiskListed &Disk, Plan &Wanted);
/* The person's files brought along, and the tags whose files stay behind. */
bool AskWhat(const Facts &Now, Plan &Wanted);
/* The new volume's name: the person's, or the machine's when they give none. */
bool AskName(const Facts &Now, Plan &Wanted);
/* Whether BoxOS starts first: asked of a disk of this machine on a UEFI start. */
bool AskFirst(const Facts &Now, Plan &Wanted);

/* The plan shown whole, with what it costs and what to know, and agreed to:
 * a whole disk signed for by its name typed, beside a yes. */
bool Agreed(const Facts &Now, const Plan &Wanted, const Reckoning &Cost);

/* Keys in hand: what was done, and the person asked whether to start it now —
 * told to take the stick out first when install ran from one (`FromStick`). */
void Handed(const Bookmark &Mark, bool FromStick);

}  // namespace install

#endif
