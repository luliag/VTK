/*=========================================================================

 Program:   Visualization Toolkit
 Module:    vtkUniformGridAMR.h

 Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
 All rights reserved.
 See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

 This software is distributed WITHOUT ANY WARRANTY; without even
 the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 PURPOSE.  See the above copyright notice for more information.

 =========================================================================*/
// .NAME vtkUniformGridAMR.h -- Abstract AMR class of uniform grids
//
// .SECTION Description
//  vtkUniformGridAMR is an abstract base class that implements common access
//  operations for AMR data.
//
// .SECTION See Also
// vtkOverlappingAMR vtkNonOverlappingAMR

#ifndef VTKUNIFORMGRIDAMR_H_
#define VTKUNIFORMGRIDAMR_H_

#include "vtkCompositeDataSet.h"

class vtkUniformGrid;
class vtkTimeStamp;

class VTK_FILTERING_EXPORT vtkUniformGridAMR : public vtkCompositeDataSet
{
  public:
    vtkTypeMacro(vtkUniformGridAMR,vtkCompositeDataSet);
    void PrintSelf(ostream& os, vtkIndent indent);

    // Description:
    // Sets the number of refinement levels.
    void SetNumberOfLevels(const unsigned int numLevels);

    // Description:
    // Return the number of levels
    unsigned int GetNumberOfLevels();

    // Description:
    // Sets the number of datasets at the given level
    void SetNumberOfDataSets(const unsigned int level, const unsigned int N);

    // Description:
    // Returns the number of datasets at the given level
    unsigned int GetNumberOfDataSets(const unsigned int level);

    // Description:
    // Returns the total number of blocks in the AMR dataset
    unsigned int GetTotalNumberOfBlocks();

    // Description:
    // Sets the dataset at the given level and index. If insufficient number
    // of levels or data slots within the level, this method will grow the
    // data-structure accordingly.
    void SetDataSet(
        const unsigned int level,const unsigned int idx,vtkUniformGrid *grid);

    // Description:
    // Appends the dataset at the given level. Increments the number of datasets
    // within the given level. Further, if an insufficient number of levels the
    // data-structure will grow accordingly.
    void AppendDataSet(const unsigned int level, vtkUniformGrid *grid);

    // Description:
    // Returns the dataset stored at the given (level,idx). The user-supplied
    // level and idx must be within the bounds of the data-structure.
    vtkUniformGrid* GetDataSet(
        const unsigned int level, const unsigned int idx);

    // Description:
    // Accessing the dataset by an iterator
    vtkUniformGrid* GetDataSet(vtkCompositeDataIterator* iter)
     {return vtkUniformGrid::SafeDownCast(this->Superclass::GetDataSet(iter));}

    // Description:
    // Retrieve the cached scalar range into the user-supplied buffer.
    void GetScalarRange(double range[2]);
    double *GetScalarRange();

    // Description:
    // Retrieve the bounds of the AMR domain
    void GetBounds(double bounds[6]);
    double *GetBounds();

  protected:
    vtkUniformGridAMR();
    virtual ~vtkUniformGridAMR();

    // Description:
    // Compute the rane of the scalars of the entire datasets and cache it into
    // ScalarRange. This method executes iff the cache is invalidated based on
    // the ScalarRangeComputTime.
    virtual void ComputeScalarRange();

    // Description:
    // Computes the bounds of the AMR dataset.
    virtual void ComputeBounds();

    double ScalarRange[2];
    vtkTimeStamp ScalarRangeComputeTime;

    double Bounds[6];

  private:
    vtkUniformGridAMR(const vtkUniformGridAMR&);//Not implemented
    void operator=(const vtkUniformGridAMR&); // Not implemented
};

#endif /* VTKUNIFORMGRIDAMR_H_ */
