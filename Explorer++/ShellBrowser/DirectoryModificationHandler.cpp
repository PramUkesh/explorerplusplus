/******************************************************************
 *
 * Project: ShellBrowser
 * File: DirectoryModificationHandler.cpp
 * License: GPL - See LICENSE in the top level directory
 *
 * Handles directory modification messages,
 * including adding, deleting and renaming
 * items.
 *
 * Written by David Erceg
 * www.explorerplusplus.com
 *
 *****************************************************************/

#include "stdafx.h"
#include <list>
#include "IShellView.h"
#include "iShellBrowser_internal.h"
#include "../Helper/Controls.h"
#include "../Helper/Helper.h"
#include "../Helper/FileOperations.h"
#include "../Helper/FolderSize.h"
#include "../Helper/ShellHelper.h"
#include "../Helper/ListViewHelper.h"
#include "../Helper/Macros.h"
#include "../Helper/Logging.h"

void CShellBrowser::StartDirectoryMonitoring(PCIDLIST_ABSOLUTE pidl)
{
	SHChangeNotifyEntry shcne;
	shcne.pidl = pidl;
	shcne.fRecursive = FALSE;
	m_shChangeNotifyId = SHChangeNotifyRegister(m_hListView, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
		SHCNE_ATTRIBUTES | SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RENAMEFOLDER | SHCNE_RENAMEITEM |
		SHCNE_RMDIR | SHCNE_UPDATEDIR | SHCNE_UPDATEITEM, WM_APP_SHELL_NOTIFY, 1, &shcne);

	if (m_shChangeNotifyId == 0)
	{
		TCHAR path[MAX_PATH];
		HRESULT hr = GetDisplayName(pidl, path, SIZEOF_ARRAY(path), SHGDN_FORPARSING);

		if (SUCCEEDED(hr))
		{
			LOG(warning) << L"Couldn't monitor directory \"" << path << L"\" for changes.";
		}
	}
}

void CShellBrowser::StopDirectoryMonitoring()
{
	if (m_shChangeNotifyId != 0)
	{
		SHChangeNotifyDeregister(m_shChangeNotifyId);
		m_shChangeNotifyId = 0;
	}
}

void CShellBrowser::OnShellNotify(WPARAM wParam, LPARAM lParam)
{
	PIDLIST_ABSOLUTE *pidls;
	LONG event;
	HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wParam), static_cast<DWORD>(lParam), &pidls, &event);

	switch (event)
	{
	case SHCNE_RENAMEFOLDER:
	case SHCNE_RENAMEITEM:
		if (ILIsParent(m_pidlDirectory, pidls[0], TRUE) && ILIsParent(m_pidlDirectory, pidls[1], TRUE))
		{
			RenameItem(pidls[0], pidls[1]);
		}
		else if (ILIsParent(m_pidlDirectory, pidls[0], TRUE))
		{
			RemoveItem(pidls[0]);
		}
		else if (ILIsParent(m_pidlDirectory, pidls[1], TRUE))
		{
			/* TODO: Add item. */
		}
		break;

	case SHCNE_RMDIR:
	case SHCNE_DELETE:
		/* Only the current directory is monitored, so notifications
		should only arrive for items in that directory. However, if
		the user has just changed directories, a notification could
		still come in for the previous directory. Therefore, it's
		important to verify that the item is actually a child of
		the current directory. */
		if (ILIsParent(m_pidlDirectory, pidls[0], TRUE))
		{
			RemoveItem(pidls[0]);
		}
		break;
	}

	SHChangeNotification_Unlock(lock);
}

void CShellBrowser::AddItem(const TCHAR *szFileName)
{
	IShellFolder	*pShellFolder = NULL;
	LPITEMIDLIST	pidlFull = NULL;
	LPITEMIDLIST	pidlRelative = NULL;
	Added_t			Added;
	TCHAR			FullFileName[MAX_PATH];
	TCHAR			szDisplayName[MAX_PATH];
	STRRET			str;
	BOOL			bFileAdded = FALSE;
	HRESULT hr;

	StringCchCopy(FullFileName,SIZEOF_ARRAY(FullFileName),m_CurDir);
	PathAppend(FullFileName,szFileName);

	hr = GetIdlFromParsingName(FullFileName,&pidlFull);

	/* It is possible that by the time a file is registered here,
	it will have already been renamed. In this the following
	check will fail.
	If the file is not added, store its filename. */
	if(SUCCEEDED(hr))
	{
		hr = SHBindToParent(pidlFull, IID_PPV_ARGS(&pShellFolder), (LPCITEMIDLIST *)&pidlRelative);

		if(SUCCEEDED(hr))
		{
			/* If this is a virtual folder, only use SHGDN_INFOLDER. If this is
			a real folder, combine SHGDN_INFOLDER with SHGDN_FORPARSING. This is
			so that items in real folders can still be shown with extensions, even
			if the global, Explorer option is disabled. */
			if(m_bVirtualFolder)
				hr = pShellFolder->GetDisplayNameOf(pidlRelative,SHGDN_INFOLDER,&str);
			else
				hr = pShellFolder->GetDisplayNameOf(pidlRelative,SHGDN_INFOLDER|SHGDN_FORPARSING,&str);

			if(SUCCEEDED(hr))
			{
				StrRetToBuf(&str,pidlRelative,szDisplayName,SIZEOF_ARRAY(szDisplayName));

				std::list<DroppedFile_t>::iterator itr;
				BOOL bDropped = FALSE;

				if(!m_DroppedFileNameList.empty())
				{
					for(itr = m_DroppedFileNameList.begin();itr != m_DroppedFileNameList.end();itr++)
					{
						if(lstrcmp(szDisplayName,itr->szFileName) == 0)
						{
							bDropped = TRUE;
							break;
						}
					}
				}

				/* Only insert the item in its sorted position if it
				wasn't dropped in. */
				if(m_bInsertSorted && !bDropped)
				{
					int iItemId;
					int iSorted;

					iItemId = SetItemInformation(m_pidlDirectory,pidlRelative,szDisplayName);

					iSorted = DetermineItemSortedPosition(iItemId);

					AddItemInternal(iSorted,iItemId,TRUE);
				}
				else
				{
					/* Just add the item to the end of the list. */
					AddItemInternal(m_pidlDirectory,pidlRelative,szDisplayName,-1,FALSE);
				}
				
				InsertAwaitingItems(m_bShowInGroups);

				bFileAdded = TRUE;
			}

			pShellFolder->Release();
		}

		CoTaskMemFree(pidlFull);
	}
	
	if(!bFileAdded)
	{
		/* The file does not exist. However, it is possible
		that is was simply renamed shortly after been created.
		Record the filename temporarily (so that it can later
		be added). */
		StringCchCopy(Added.szFileName,SIZEOF_ARRAY(Added.szFileName),szFileName);
		m_FilesAdded.push_back(Added);
	}
}

void CShellBrowser::RemoveItem(PCIDLIST_ABSOLUTE pidl)
{
	int iItemInternal = LocateFileItemInternalIndex(pidl);

	if (iItemInternal != -1)
	{
		RemoveItem(iItemInternal);
	}
}

void CShellBrowser::RemoveItem(int iItemInternal)
{
	ULARGE_INTEGER	ulFileSize;
	LVFINDINFO		lvfi;
	BOOL			bFolder;
	int				iItem;
	int				nItems;

	if (iItemInternal == -1)
		return;

	CoTaskMemFree(m_pExtraItemInfo[iItemInternal].pridl);

	/* Is this item a folder? */
	bFolder = (m_pwfdFiles[iItemInternal].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ==
		FILE_ATTRIBUTE_DIRECTORY;

	/* Take the file size of the removed file away from the total
	directory size. */
	ulFileSize.LowPart = m_pwfdFiles[iItemInternal].nFileSizeLow;
	ulFileSize.HighPart = m_pwfdFiles[iItemInternal].nFileSizeHigh;

	m_ulTotalDirSize.QuadPart -= ulFileSize.QuadPart;

	/* Locate the item within the listview.
	Could use filename, providing removed
	items are always deleted before new
	items are inserted. */
	lvfi.flags = LVFI_PARAM;
	lvfi.lParam = iItemInternal;
	iItem = ListView_FindItem(m_hListView, -1, &lvfi);

	if (iItem != -1)
	{
		/* Remove the item from the listview. */
		ListView_DeleteItem(m_hListView, iItem);
	}

	/* Invalidate the items internal data.
	This will mark it as free, so that it
	can be used by another item. */
	m_pItemMap[iItemInternal] = 0;

	nItems = ListView_GetItemCount(m_hListView);

	m_nTotalItems--;

	if (nItems == 0 && !m_bApplyFilter)
	{
		SendMessage(m_hOwner, WM_USER_FOLDEREMPTY, m_ID, TRUE);
	}
}

/*
 * Modifies the attributes of an item currently in the listview.
 */
void CShellBrowser::ModifyItem(const TCHAR *FileName)
{
	HANDLE			hFirstFile;
	ULARGE_INTEGER	ulFileSize;
	LVITEM			lvItem;
	TCHAR			FullFileName[MAX_PATH];
	BOOL			bFolder;
	BOOL			res;
	int				iItem;
	int				iItemInternal = -1;

	iItem = LocateFileItemIndex(FileName);

	/* Although an item may not have been added to the listview
	yet, it is critical that its' size still be updated if
	necessary.
	It is possible (and quite likely) that the file add and
	modified messages will be sent in the same group, meaning
	that when the modification message is processed, the item
	is not in the listview, but it still needs to be updated.
	Therefore, instead of searching for items solely in the
	listview, also look through the list of pending file
	additions. */

	if(iItem == -1)
	{
		/* The item doesn't exist in the listview. This can
		happen when a file has been created with a non-zero
		size, but an item has not yet been inserted into
		the listview.
		Search through the list of items waiting to be
		inserted, so that files the have just been created
		can be updated without them residing within the
		listview. */
		std::list<AwaitingAdd_t>::iterator itr;

		for(itr = m_AwaitingAddList.begin();itr!= m_AwaitingAddList.end();itr++)
		{
			if(lstrcmp(m_pwfdFiles[itr->iItemInternal].cFileName,FileName) == 0)
			{
				iItemInternal = itr->iItemInternal;
				break;
			}
		}
	}
	else
	{
		/* The item exists in the listview. Determine its
		internal index from its listview information. */
		lvItem.mask		= LVIF_PARAM;
		lvItem.iItem	= iItem;
		lvItem.iSubItem	= 0;
		res = ListView_GetItem(m_hListView,&lvItem);

		if(res != FALSE)
			iItemInternal = (int)lvItem.lParam;

		TCHAR szFullFileName[MAX_PATH];
		StringCchCopy(szFullFileName,SIZEOF_ARRAY(szFullFileName),m_CurDir);
		PathAppend(szFullFileName,FileName);

		/* When a file is modified, its icon overlay may change.
		This is the case when modifying a file managed by
		TortoiseSVN, for example. */
		SHFILEINFO shfi;
		DWORD_PTR dwRes = SHGetFileInfo(szFullFileName,0,&shfi,sizeof(SHFILEINFO),SHGFI_ICON|SHGFI_OVERLAYINDEX);

		if(dwRes != 0)
		{
			lvItem.mask			= LVIF_STATE;
			lvItem.iItem		= iItem;
			lvItem.iSubItem		= 0;
			lvItem.stateMask	= LVIS_OVERLAYMASK;
			lvItem.state		= INDEXTOOVERLAYMASK(shfi.iIcon >> 24);
			ListView_SetItem(m_hListView,&lvItem);

			DestroyIcon(shfi.hIcon);
		}
	}

	if(iItemInternal != -1)
	{
		/* Is this item a folder? */
		bFolder = (m_pwfdFiles[iItemInternal].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ==
			FILE_ATTRIBUTE_DIRECTORY;

		ulFileSize.LowPart = m_pwfdFiles[iItemInternal].nFileSizeLow;
		ulFileSize.HighPart = m_pwfdFiles[iItemInternal].nFileSizeHigh;

		m_ulTotalDirSize.QuadPart -= ulFileSize.QuadPart;

		if(ListView_GetItemState(m_hListView,iItem,LVIS_SELECTED)
		== LVIS_SELECTED)
		{
			ulFileSize.LowPart = m_pwfdFiles[iItemInternal].nFileSizeLow;
			ulFileSize.HighPart = m_pwfdFiles[iItemInternal].nFileSizeHigh;

			m_ulFileSelectionSize.QuadPart -= ulFileSize.QuadPart;
		}

		StringCchCopy(FullFileName,SIZEOF_ARRAY(FullFileName),m_CurDir);
		PathAppend(FullFileName,FileName);

		hFirstFile = FindFirstFile(FullFileName,&m_pwfdFiles[iItemInternal]);

		if(hFirstFile != INVALID_HANDLE_VALUE)
		{
			ulFileSize.LowPart = m_pwfdFiles[iItemInternal].nFileSizeLow;
			ulFileSize.HighPart = m_pwfdFiles[iItemInternal].nFileSizeHigh;

			m_ulTotalDirSize.QuadPart += ulFileSize.QuadPart;

			if(ListView_GetItemState(m_hListView,iItem,LVIS_SELECTED)
				== LVIS_SELECTED)
			{
				ulFileSize.LowPart = m_pwfdFiles[iItemInternal].nFileSizeLow;
				ulFileSize.HighPart = m_pwfdFiles[iItemInternal].nFileSizeHigh;

				m_ulFileSelectionSize.QuadPart += ulFileSize.QuadPart;
			}

			if((m_pwfdFiles[iItemInternal].dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ==
				FILE_ATTRIBUTE_HIDDEN)
			{
				ListView_SetItemState(m_hListView,iItem,LVIS_CUT,LVIS_CUT);
			}
			else
				ListView_SetItemState(m_hListView,iItem,0,LVIS_CUT);

			if(m_ViewMode == VM_DETAILS)
			{
				std::list<Column_t>::iterator itrColumn;
				int iColumnIndex = 0;

				if(m_pActiveColumnList != NULL)
				{
					for(itrColumn = m_pActiveColumnList->begin();itrColumn != m_pActiveColumnList->end();itrColumn++)
					{
						if(itrColumn->bChecked)
						{
							SetColumnText(itrColumn->id,iItem,iColumnIndex++);
						}
					}
				}
			}

			FindClose(hFirstFile);
		}
		else
		{
			/* The file may not exist if, for example, it was
			renamed just after a file with the same name was
			deleted. If this does happen, a modification
			message will likely be sent out after the file
			has been renamed, indicating the new items properties.
			However, the files' size will be subtracted on
			modification. If the internal structures still hold
			the old size, the total directory size will become
			corrupted. */
			m_pwfdFiles[iItemInternal].nFileSizeLow		= 0;
			m_pwfdFiles[iItemInternal].nFileSizeHigh	= 0;
		}
	}
}

/* Renames an item currently in the listview. */
/* TODO: This code should be coalesced with the code that
adds items as well as the code that finds their icons.
ALL changes to an items name/internal properties/icon/overlay icon
should go through a central function. */
void CShellBrowser::RenameItem(PCIDLIST_ABSOLUTE pidlOld, PCIDLIST_ABSOLUTE pidlNew)
{
	IShellFolder	*pShellFolder = NULL;
	LPITEMIDLIST	pidlRelative = NULL;
	SHFILEINFO		shfi;
	LVFINDINFO		lvfi;
	TCHAR			szDisplayName[MAX_PATH];
	LVITEM			lvItem;
	DWORD_PTR		res;
	HRESULT			hr;
	int				iItem;

	int iItemInternal = LocateFileItemInternalIndex(pidlOld);

	if (iItemInternal == -1)
	{
		return;
	}

	hr = SHBindToParent(pidlNew, IID_PPV_ARGS(&pShellFolder), (LPCITEMIDLIST *) &pidlRelative);

	if(SUCCEEDED(hr))
	{
		hr = GetDisplayName(pidlNew,szDisplayName,SIZEOF_ARRAY(szDisplayName),SHGDN_INFOLDER|SHGDN_FORPARSING);

		if(SUCCEEDED(hr))
		{
			m_pExtraItemInfo[iItemInternal].pridl = ILClone(pidlRelative);
			StringCchCopy(m_pExtraItemInfo[iItemInternal].szDisplayName,
				SIZEOF_ARRAY(m_pExtraItemInfo[iItemInternal].szDisplayName),
				szDisplayName);

			/* Need to update internal storage for the item, since
			it's name has now changed. */
			StringCchCopy(m_pwfdFiles[iItemInternal].cFileName,
				SIZEOF_ARRAY(m_pwfdFiles[iItemInternal].cFileName),
				szDisplayName);

			/* The files' type may have changed, so retrieve the files'
			icon again. */
			res = SHGetFileInfo((LPTSTR)pidlNew,0,&shfi,
				sizeof(SHFILEINFO),SHGFI_PIDL|SHGFI_ICON|
				SHGFI_OVERLAYINDEX);

			if(res != 0)
			{
				/* Locate the item within the listview. */
				lvfi.flags	= LVFI_PARAM;
				lvfi.lParam	= iItemInternal;
				iItem = ListView_FindItem(m_hListView,-1,&lvfi);

				if(iItem != -1)
				{
					lvItem.mask			= LVIF_TEXT|LVIF_IMAGE|LVIF_STATE;
					lvItem.iItem		= iItem;
					lvItem.iSubItem		= 0;
					lvItem.iImage		= shfi.iIcon;
					lvItem.pszText		= ProcessItemFileName(iItemInternal);
					lvItem.stateMask	= LVIS_OVERLAYMASK;

					/* As well as resetting the items icon, we'll also set
					it's overlay again (the overlay could change, for example,
					if the file is changed to a shortcut). */
					lvItem.state		= INDEXTOOVERLAYMASK(shfi.iIcon >> 24);

					/* Update the item in the listview. */
					ListView_SetItem(m_hListView,&lvItem);

					/* TODO: Does the file need to be filtered out? */
					if(IsFileFiltered(iItemInternal))
					{
						RemoveFilteredItem(iItem,iItemInternal);
					}
				}

				DestroyIcon(shfi.hIcon);
			}
		}

		pShellFolder->Release();
	}
}