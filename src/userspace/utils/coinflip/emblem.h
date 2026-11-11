/* emblem.h — what is struck on the two faces: a crowned head in profile on
 * heads, a tower on tails. Each is drawn in the coin's own units, the face
 * a disc of radius 1, and says whether a point of it is raised. */
#ifndef COINFLIP_EMBLEM_H
#define COINFLIP_EMBLEM_H

namespace coinflip {

// A and B are across and up the face, as one reads it.
bool HeadsRaised(double A, double B);
bool TailsRaised(double A, double B);

}  // namespace coinflip

#endif
